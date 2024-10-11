// Copyright (c) Yevhenii Selivanov

#include "LSATUtilsLibrary.h"
//---
#include "Data/LSATTrimTimesData.h"
//---
#include "AssetExportTask.h"
#include "LevelSequence.h"
#include "LevelSequencerAudioTrimmerEdModule.h"
#include "LSATSettings.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Exporters/Exporter.h"
#include "Factories/ReimportSoundFactory.h"
#include "HAL/FileManager.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sound/SampleBufferIO.h"
#include "Sound/SoundWave.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tracks/MovieSceneAudioTrack.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LSATUtilsLibrary)

// Entry method to run the main flow of trimming all audio assets for the given level sequence
void ULSATUtilsLibrary::RunLevelSequenceAudioTrimmer(const TArray<ULevelSequence*>& LevelSequences)
{
	/*********************************************************************************************
	 * Gathering: Prepares the `TrimTimesMultiMap` map that combines sound waves with their corresponding trim times.
	 *********************************************************************************************
	 * 1. GatherSoundsInRequestedLevelSequence ➔ Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence.
	 * 2. GatherSoundsInOtherSequences ➔ Handles those sounds from original Level Sequence that are used at the same time in other Level Sequences.
	 * 3. GatherSoundsOutsideSequences ➔ Handle sound waves that are used outside of level sequences like in the world or blueprints.
	 ********************************************************************************************* */

	FLSATTrimTimesMultiMap TrimTimesMultiMap;

	for (const ULevelSequence* LevelSequence : LevelSequences)
	{
		GatherSoundsInRequestedLevelSequence(/*out*/TrimTimesMultiMap, LevelSequence);
		GatherSoundsInOtherSequences(/*InOut*/TrimTimesMultiMap);
		GatherSoundsOutsideSequences(/*InOut*/TrimTimesMultiMap);
	}

	/*********************************************************************************************
	 * Preprocessing: Initial modifying of the `TrimTimesMultiMap` map based on the gathered sounds.
	 *********************************************************************************************
	 * 1. HandleTrackBoundaries ➔ Trims the audio tracks by level sequence boundaries, so the audio is not played outside of the level sequence.
	 * 2. HandleLargeStartOffset ➔ Handles cases where the start offset is larger than the total length of the audio.
	 * 3. HandlePolicyLoopingSounds ➔ Handles the policy for looping sounds based on the settings, e.g: skipping all looping sounds.
	 * 4. HandlePolicySegmentsReuse ➔ Handles the reuse and fragmentation of sound segments within a level sequence.
	 ********************************************************************************************* */

	if (TrimTimesMultiMap.IsEmpty())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: No valid sound waves found for trimming."), __FUNCTION__);
		return;
	}

	HandleTrackBoundaries(/*InOut*/TrimTimesMultiMap);
	HandleLargeStartOffset(/*InOut*/TrimTimesMultiMap);
	HandlePolicyLoopingSounds(/*InOut*/TrimTimesMultiMap);
	HandlePolicySegmentsReuse(/*InOut*/TrimTimesMultiMap);

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Found %d unique sound waves with valid trim times."), __FUNCTION__, TrimTimesMultiMap.Num());

	/*********************************************************************************************
	 * Main Flow: Is called after the preprocessing for each found audio.
	 *********************************************************************************************
	 * 
	 * [Example Data] - Let's assume we have the following sounds and audio sections in the level sequence:
	 * - SW_Ball is used twice: AudioSection0[15-30], AudioSection1[15-30]
	 * - SW_Step is used three times: AudioSection2[7-10], AudioSection3[7-10], AudioSection4[18-25]
	 * 
	 * [TrimTimesMultiMap] - Illustrates how Example Data is iterated and processed:
	 * |
	 * |-- SW_Ball
	 * |    |-- [15-30] 
	 * |        |-- AudioSection0  -> Trim and reimport directly to SW_Ball
	 * |        |-- AudioSection1  -> Reuse trimmed SW_Ball
	 * |
	 * |-- SW_Step
	 *	 	 |-- [7-10] -> Duplicate to SW_Step1, so it won't break next [18-25]
	 *		 |    |-- AudioSection2  -> Trim and reimport to duplicated SW_Step1
	 *		 |    |-- AudioSection3  -> Reuse trimmed SW_Step1
	 *		 |
	 *		 |-- [18-25]
	 *			  |-- AudioSection4 -> Trim and reimport directly to SW_Step
	 *
	 * [Main Flow] - Is called after the preprocessing for each found audio:
	 * 1. ExportSoundWaveToWav ➔ Convert sound wave into a WAV file.
	 * 2. TrimAudio ➔ Apply trimming to the WAV file.
	 * 3. ReimportAudioToUnreal ➔ Load the trimmed WAV file back into the engine.
	 * 4. ResetTrimmedAudioSection ➔ Update audio section with the new sound.
	 * 5. DeleteTempWavFile ➔ Remove the temporary WAV file.
	 ******************************************************************************************* */

	const ELSATPolicyDifferentTrimTimes PolicyDifferentTrimTimes = ULSATSettings::Get().PolicyDifferentTrimTimes;
	for (const TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& OuterIt : TrimTimesMultiMap)
	{
		USoundWave* const OriginalSoundWave = OuterIt.Key;
		const FLSATTrimTimesMap& InnerMap = OuterIt.Value;

		// Handle the SkipAll policy: Skip processing for all tracks if there are different trim times
		if (InnerMap.Num() > 1
			&& PolicyDifferentTrimTimes == ELSATPolicyDifferentTrimTimes::SkipAll)
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Skipping processing for sound wave %s due to different trim times."), __FUNCTION__, *GetNameSafe(OriginalSoundWave));
			continue;
		}

		int32 GroupIndex = 0;
		for (const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& InnerIt : InnerMap)
		{
			const FLSATTrimTimes& TrimTimes = InnerIt.Key;
			const FLSATSectionsContainer& Sections = InnerIt.Value;
			USoundWave* TrimmedSoundWave = OriginalSoundWave;

			if (TrimTimes.IsSoundTrimmed())
			{
				UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Skipping export for audio %s as there is almost no difference between total duration and usage duration"), __FUNCTION__, *GetNameSafe(TrimTimes.GetSoundWave()));
				continue;
			}

			const bool bIsBeforeLastGroup = GroupIndex < InnerMap.Num() - 1;
			if (bIsBeforeLastGroup
				&& PolicyDifferentTrimTimes == ELSATPolicyDifferentTrimTimes::ReimportOneAndDuplicateOthers)
			{
				/* Duplicate sound wave for different timings, so trimmed sound will be reimported in the duplicated sound wave
				 *	SW_Step
				 *	 |-- [7-10] -> HERE: duplicate to SW_Step1, so it won't break next SW_Step[18-25]
				 *	 |-- [18-25] */
				TrimmedSoundWave = DuplicateSoundWave(TrimmedSoundWave, GroupIndex + 1);

				// Fall through to process duplicated sound wave
			}

			// Process only the first section in this group
			bool bReuseFurtherSections = false;
			for (UMovieSceneAudioSection* Section : Sections)
			{
				if (bReuseFurtherSections)
				{
					/* No need to fully process other sections, just reuse already trimmed sound wave
					* |-- AudioSection0  -> Trim
					* |-- AudioSection1  -> HERE: Reuse trimmed */
					ResetTrimmedAudioSection(Section, TrimmedSoundWave);
					continue;
				}

				// Export the sound wave to a temporary WAV file
				const FString ExportPath = ExportSoundWaveToWav(TrimmedSoundWave);
				if (ExportPath.IsEmpty())
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Failed to export %s"), __FUNCTION__, *TrimmedSoundWave->GetName());
					continue;
				}

				// Perform the audio trimming
				const FString TrimmedAudioPath = FPaths::ChangeExtension(ExportPath, TEXT("_trimmed.wav"));
				const bool bSuccessTrim = TrimAudio(TrimTimes, ExportPath, TrimmedAudioPath);
				if (!bSuccessTrim)
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Trimming audio failed for %s"), __FUNCTION__, *TrimmedSoundWave->GetName());
					continue;
				}

				// Reimport the trimmed audio back into Unreal Engine
				const bool bSuccessReimport = ReimportAudioToUnreal(TrimmedSoundWave, TrimmedAudioPath);
				if (!bSuccessReimport)
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Reimporting trimmed audio failed for %s"), __FUNCTION__, *TrimmedSoundWave->GetName());
					continue;
				}

				// Reset the start frame offset for this an audio section
				ResetTrimmedAudioSection(Section, TrimmedSoundWave);

				// Delete the temporary exported WAV file
				DeleteTempWavFile(ExportPath);
				DeleteTempWavFile(TrimmedAudioPath);

				// Mark that the first section has been processed
				bReuseFurtherSections = true;
			}

			GroupIndex++;
		}
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Processing complete."), __FUNCTION__);
}

/*********************************************************************************************
 * Preprocessing
 ********************************************************************************************* */

// Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence
void ULSATUtilsLibrary::GatherSoundsInRequestedLevelSequence(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap, const ULevelSequence* LevelSequence)
{
	if (!ensureMsgf(LevelSequence, TEXT("ASSERT: [%i] %hs:\n'LevelSequence' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	// Retrieve audio sections mapped by SoundWave from the main Level Sequence
	TMap<USoundWave*, FLSATSectionsContainer> MainAudioSectionsMap;
	FindAudioSectionsInLevelSequence(MainAudioSectionsMap, LevelSequence);

	if (MainAudioSectionsMap.IsEmpty())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: No audio sections found in the level sequence."), __FUNCTION__);
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Found %d unique sound waves in the main sequence."), __FUNCTION__, MainAudioSectionsMap.Num());

	for (const TTuple<USoundWave*, FLSATSectionsContainer>& It : MainAudioSectionsMap)
	{
		USoundWave* OriginalSoundWave = It.Key;
		const FLSATSectionsContainer& MainSections = It.Value;

		// Calculate and combine trim times for the main sequence
		FLSATTrimTimesMap& TrimTimesMap = InOutTrimTimesMultiMap.FindOrAdd(OriginalSoundWave);
		CalculateTrimTimesInAllSections(TrimTimesMap, MainSections);
	}
}

// Handles those sounds from requested Level Sequence that are used at the same time in other Level Sequences
void ULSATUtilsLibrary::GatherSoundsInOtherSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	if (InOutTrimTimesMultiMap.IsEmpty())
	{
		return;
	}

	for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& ItRef : InOutTrimTimesMultiMap)
	{
		const USoundWave* OriginalSoundWave = ItRef.Key;
		if (!OriginalSoundWave)
		{
			continue;
		}

		// Get first level sequence where the sound wave is used as iterator from the map
		const ULevelSequence* OriginalLevelSequence = GetLevelSequence(ItRef.Value.GetFirstAudioSection());
		if (!OriginalLevelSequence)
		{
			continue;
		}

		// Find other level sequences where the sound wave is used
		TArray<UObject*> OutUsages;
		FindAudioUsagesBySoundAsset(OutUsages, OriginalSoundWave);

		for (UObject* Usage : OutUsages)
		{
			const ULevelSequence* OtherLevelSequence = Cast<ULevelSequence>(Usage);
			if (!OtherLevelSequence
				|| OtherLevelSequence == OriginalLevelSequence)
			{
				continue;
			}

			UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Found sound wave '%s' usage in other level sequence: %s, its sections will be processed as well"), __FUNCTION__, *OriginalSoundWave->GetName(), *OtherLevelSequence->GetName());

			// Retrieve audio sections for the sound wave in the other level sequence
			TMap<USoundWave*, FLSATSectionsContainer> OtherAudioSectionsMap;
			FindAudioSectionsInLevelSequence(OtherAudioSectionsMap, OtherLevelSequence);

			if (const FLSATSectionsContainer* OtherSectionsPtr = OtherAudioSectionsMap.Find(OriginalSoundWave))
			{
				// Calculate and combine trim times for the other level sequences
				CalculateTrimTimesInAllSections(ItRef.Value, *OtherSectionsPtr);
			}
		}
	}
}

// Trims the audio tracks by level sequence boundaries, so the audio is not played outside of the level sequence
void ULSATUtilsLibrary::HandleTrackBoundaries(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& SoundWaveEntry : InOutTrimTimesMultiMap)
	{
		FLSATTrimTimesMap& TrimTimesMapRef = SoundWaveEntry.Value;
		TrimTimesMapRef.RebuildTrimTimesMapWithProcessor([&](UMovieSceneAudioSection* AudioSection, const FLSATTrimTimes& TrimTimes, FLSATSectionsContainer& OutAllNewSections)
		{
			const ULevelSequence* LevelSequence = GetLevelSequence(AudioSection);
			if (!LevelSequence)
			{
				return;
			}

			const FFrameRate TickResolution = GetTickResolution(AudioSection);
			const FFrameNumber LevelSequenceStartFrame = LevelSequence->GetMovieScene()->GetPlaybackRange().GetLowerBoundValue();
			const FFrameNumber LevelSequenceEndFrame = LevelSequence->GetMovieScene()->GetPlaybackRange().GetUpperBoundValue();
			const int32 LevelSequenceStartMs = ConvertFrameToMs(LevelSequenceStartFrame, TickResolution);
			const int32 LevelSequenceEndMs = ConvertFrameToMs(LevelSequenceEndFrame, TickResolution);

			// Get the actual start and end times of the audio section in the level sequence
			const int32 SectionStartMs = ConvertFrameToMs(AudioSection->GetInclusiveStartFrame(), TickResolution);
			const int32 SectionEndMs = ConvertFrameToMs(AudioSection->GetExclusiveEndFrame(), TickResolution);

			// Adjust trim times if they are outside the level sequence boundaries
			bool bTrimmed = false;
			int32 AdjustedTrimStartMs = TrimTimes.GetSoundTrimStartMs();
			FFrameNumber NewStartFrame = AudioSection->GetInclusiveStartFrame();
			FFrameNumber NewEndFrame = AudioSection->GetExclusiveEndFrame();

			if (SectionStartMs < LevelSequenceStartMs)
			{
				// Adjust SoundTrimStartMs by considering the local offset of the sound within the level sequence boundary
				const int32 ExcessDurationMs = LevelSequenceStartMs - SectionStartMs;
				AdjustedTrimStartMs += ExcessDurationMs;
				NewStartFrame = LevelSequenceStartFrame;
				bTrimmed = true;
			}

			if (SectionEndMs > LevelSequenceEndMs)
			{
				NewEndFrame = LevelSequenceEndFrame;
				bTrimmed = true;
			}

			if (!bTrimmed)
			{
				// The audio track is within the level sequence boundaries
				return;
			}

			constexpr bool bIsLeftTrim = true;
			constexpr bool bDeleteKeys = false;

			// Apply trimming to the left side of the section if it starts before the level sequence start frame
			if (AudioSection->GetInclusiveStartFrame() < LevelSequenceStartFrame)
			{
				AudioSection->TrimSection(FQualifiedFrameTime(NewStartFrame, TickResolution), bIsLeftTrim, bDeleteKeys);
				AudioSection->SetStartOffset(ConvertMsToFrameNumber(AdjustedTrimStartMs, TickResolution));
				UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Trimmed left side of section '%s'"), __FUNCTION__, *AudioSection->GetName());
			}

			// Apply trimming to the right side of the section if it ends after the level sequence end frame
			if (AudioSection->GetExclusiveEndFrame() > LevelSequenceEndFrame)
			{
				AudioSection->TrimSection(FQualifiedFrameTime(NewEndFrame, TickResolution), !bIsLeftTrim, bDeleteKeys);
				UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Trimmed right side of section '%s'"), __FUNCTION__, *AudioSection->GetName());
			}

			OutAllNewSections.Add(AudioSection);
			UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Finished trim to boundaries the section '%s' | %s"), __FUNCTION__, *AudioSection->GetName(), *TrimTimes.ToString(TickResolution));
		});
	}
}

// Handles cases where the start offset is larger than the total length of the audio
void ULSATUtilsLibrary::HandleLargeStartOffset(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& SoundWaveEntry : InOutTrimTimesMultiMap)
	{
		FLSATTrimTimesMap& TrimTimesMap = SoundWaveEntry.Value;
		TrimTimesMap.RebuildTrimTimesMapWithProcessor([&](UMovieSceneAudioSection* AudioSection, const FLSATTrimTimes& TrimTimes, FLSATSectionsContainer& OutAllNewSections)
		{
			const FFrameRate TickResolution = GetTickResolution(AudioSection);
			if (!TickResolution.IsValid())
			{
				UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: TickResolution is not valid for audio section '%s'"), __FUNCTION__, *AudioSection->GetName());
				return;
			}

			const int32 TotalSoundDurationMs = TrimTimes.GetSoundTotalDurationMs();
			if (TrimTimes.GetSoundTrimStartMs() < TotalSoundDurationMs)
			{
				// Is within
				return;
			}

			// Adjust the start offset if it is larger than the entire sound duration
			const int32 NewSoundTrimStartMs = TrimTimes.GetSoundTrimStartMs() % TotalSoundDurationMs;
			AudioSection->SetStartOffset(ConvertMsToFrameNumber(NewSoundTrimStartMs, TickResolution));

			OutAllNewSections.Add(AudioSection);
			UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Start offset is larger than duration for section '%s'. Adjusted StartOffset to: %d ms."), __FUNCTION__, *AudioSection->GetName(), TrimTimes.GetSoundTrimStartMs());
		});
	}
}

// Handles the policy for looping sounds based on the settings, e.g: skipping all looping sounds or splitting them
void ULSATUtilsLibrary::HandlePolicyLoopingSounds(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	TArray<USoundWave*> LoopingSounds;
	InOutTrimTimesMultiMap.GetSounds(LoopingSounds, [](const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& It)
	{
		if (It.Key.IsLooping())
		{
			UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Found looping %s"), __FUNCTION__, *It.Key.ToCompactString());
			return true;
		}
		return false;
	});

	if (LoopingSounds.IsEmpty())
	{
		return;
	}

	switch (ULSATSettings::Get().PolicyLoopingSounds)
	{
	case ELSATPolicyLoopingSounds::SkipAll:
		// Looping sounds should not be processed at all for this and all other audio tracks that use the same sound wave
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Skip processing all looping sounds according to the looping policy"), __FUNCTION__);
		InOutTrimTimesMultiMap.Remove(LoopingSounds);
		break;

	case ELSATPolicyLoopingSounds::SkipAndDuplicate:
		/* Section with looping sound will not be processed, but all other usages of the same sound wave will be duplicated into separate sound wave asset
		 * SW_Background
		 *	 |-- [3-12] -> Duplicate to SW_Background1, move [3-12] Trim Times from SW_Background to SW_Background1
		 *	 |    |-- AudioSection0  -> Change to duplicated SW_Background1
		 *	 |    |-- AudioSection1  -> Change to duplicated SW_Background1
		 *	 |
		 *	 |-- [74-15] -> Is looping, starts from 74 and ends at 15 
		 *		  |-- AudioSection2 -> Skip: will be removed from the multimap
		 */
		for (USoundWave* LoopingSound : LoopingSounds)
		{
			USoundWave* DuplicatedSound = nullptr;
			FLSATTrimTimesMap& TrimTimesMap = InOutTrimTimesMultiMap.FindOrAdd(LoopingSound);
			TrimTimesMap.RebuildTrimTimesMapWithProcessor([&](UMovieSceneAudioSection* AudioSection, const FLSATTrimTimes& TrimTimes, FLSATSectionsContainer& OutAllNewSections)
			{
				if (TrimTimes.IsLooping())
				{
					// Skip the looping sections
					return;
				}

				// Duplicate the sound wave for non-looping sections if not already done
				if (!DuplicatedSound)
				{
					DuplicatedSound = DuplicateSoundWave(LoopingSound);
					OutAllNewSections.Add(AudioSection);
				}
			});
		}
		break;

	case ELSATPolicyLoopingSounds::SplitSections:
		/* Splits looping sections into multiple segments based on the total duration of the sound asset.
		 *
		 * |===============|========|========|
		 *     ^               ^        ^
		 * Base Segment     Loop 1   Loop 2
		 *
		 * [BEFORE]
		 * |-- [0-75] -> This is looping and exceeds the total sound duration of 30ms
		 * |    |-- AudioSection0
		 * 
		 * [AFTER]
		 *   |-- [0-30] -> First segment
		 *   |    |-- AudioSection0 (split part 1)
		 *   |
		 *   |-- [30-60] -> Second segment
		 *   |    |-- AudioSection0 (split part 2)
		 *   |
		 *   |-- [60-75] -> Third segment
		 *   |    |-- AudioSection0 (split part 3)
		 */
		for (USoundWave* LoopingSound : LoopingSounds)
		{
			FLSATTrimTimesMap& TrimTimesMapRef = InOutTrimTimesMultiMap.FindOrAdd(LoopingSound);

			TrimTimesMapRef.RebuildTrimTimesMapWithProcessor([](UMovieSceneAudioSection* AudioSection, const FLSATTrimTimes& TrimTimes, FLSATSectionsContainer& OutAllNewSections)
			{
				if (!TrimTimes.IsLooping())
				{
					return;
				}

				FLSATSectionsContainer SplitSections;
				SplitLoopingSection(/*out*/SplitSections, AudioSection, TrimTimes);

				OutAllNewSections.Append(SplitSections);
			});
		}
		break;

	default:
		ensureMsgf(false, TEXT("ERROR: [%i] %hs:\nUnhandled PolicyLoopingSounds value!"), __LINE__, __FUNCTION__);
	}
}

// Main goal of this function is to handle those sounds that are used outside of level sequences like in the world or blueprints
// Handles the policy for sounds used outside of level sequences, e.g., skipping or duplicating them.
void ULSATUtilsLibrary::GatherSoundsOutsideSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	if (InOutTrimTimesMultiMap.IsEmpty())
	{
		return;
	}

	TArray<USoundWave*> SoundsOutsideSequences;
	InOutTrimTimesMultiMap.GetSounds(SoundsOutsideSequences, [](const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& It)
	{
		// Find other usages of the sound wave outside the level sequences.
		TArray<UObject*> OutUsages;
		FindAudioUsagesBySoundAsset(OutUsages, It.Key.GetSoundWave());

		const bool bHasExternalUsages = OutUsages.ContainsByPredicate([](const UObject* Usage) { return Usage && !Usage->IsA<ULevelSequence>(); });
		if (bHasExternalUsages)
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Sound wave '%s' is used outside of level sequences by different assets (like in the world or blueprints)"
				       ", `Sounds Outside Sequences` Policy will be applied"), __FUNCTION__, *GetNameSafe(It.Key.GetSoundWave()));
		}
		return bHasExternalUsages;
	});

	if (SoundsOutsideSequences.IsEmpty())
	{
		return;
	}

	switch (ULSATSettings::Get().PolicySoundsOutsideSequences)
	{
	case ELSATPolicySoundsOutsideSequences::SkipAll:
		// This sound wave will not be processed at all if it's used anywhere outside level sequences
		InOutTrimTimesMultiMap.Remove(SoundsOutsideSequences);
		break;

	case ELSATPolicySoundsOutsideSequences::SkipAndDuplicate:
		/* Duplicate the sound wave and replace the original sound wave with the duplicated one in the multimap.
		 * SW_Wind
		 *   |-- [15-30] -> Duplicate to SW_Wind1, move [15-30] Trim Times from SW_Wind to SW_Wind1
		 *   |    |-- AudioSection0  -> Change to duplicated SW_Wind1
		 *   |    |-- AudioSection1  -> Change to duplicated SW_Wind1
		 *   |
		 *   |-- ExternalUsage -> Found in Blueprint 'BP_Environment' -> Skip: SW_Wind remains untouched
		 */
		for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& ItRef : InOutTrimTimesMultiMap)
		{
			if (!SoundsOutsideSequences.Contains(ItRef.Key))
			{
				continue;
			}

			// We don't want to break other usages in levels and blueprints of this sound by reimporting with new timings
			// Therefore, duplicate it and replace the original sound wave with the duplicated one in given map
			USoundWave* DuplicatedSoundWave = DuplicateSoundWave(ItRef.Key);
			ItRef.Key = DuplicatedSoundWave;
			ItRef.Value.SetSound(DuplicatedSoundWave);
		}
		break;

	default:
		ensureMsgf(false, TEXT("ERROR: [%i] %hs:\nUnhandled PolicySoundsOutsideSequences value!"), __LINE__, __FUNCTION__);
	}
}

// Handles the reuse and fragmentation of sound segments within a level sequence
void ULSATUtilsLibrary::HandlePolicySegmentsReuse(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	switch (ULSATSettings::Get().PolicySegmentsReuse)
	{
	case ELSATPolicySegmentsReuse::KeepOriginal:
		// Segments will not be fragmented and reused, but kept as original
		break;

	case ELSATPolicySegmentsReuse::SplitToSmaller:
		/** Segments will be fragmented into smaller reusable parts, with each usage sharing overlapping segments.
		 * 
		 * [BEFORE]
		 * 
		 * |===3===|=====4=======|===5===|
		 *     ^         ^           ^
		 *   [4-5]     [0-5]       [0-1]
		 *
		 * |-- [4-5] - [167ms-208ms]
		 * |    |-- AudioSection_3
		 * |
		 * |-- [0-5] - [0ms-209ms]
		 * |    |-- AudioSection_4
		 * |
		 * |-- [0-1] - [0ms-41ms]
		 * |    |-- AudioSection_5
		 * 
		 * [AFTER]
		 * 
		 * |===0===|===1===|===2===|===6===|===7===|
		 *     ^       ^       ^       ^       ^
		 *   [4-5]   [0-1]   [1-4]   [4-5]   [0-1]
		 *
		 *   |-- [4-5] - [167ms-208ms] -> Reused in two sections
		 *   |    |-- AudioSection_0
		 *   |    |-- AudioSection_6
		 *   |
		 *   |-- [1-4] - [41ms-167ms] -> New segment from middle
		 *   |    |-- AudioSection_2
		 *   |
		 *   |-- [0-1] - [0ms-41ms] -> Reused in two sections 
		 *   |    |-- AudioSection_1
		 *   |    |-- AudioSection_7
		 * 
		 * In the [AFTER] visualization, the original segment [0-5] has been split into smaller parts: [0-1], [1-4], and [4-5].
		 * Reused parts, such as [4-5] and [0-1], now have multiple audio sections referencing them, while the middle part [1-4] has been newly created.
		 */
		for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& SoundWaveEntry : InOutTrimTimesMultiMap)
		{
			USoundWave* SoundWave = SoundWaveEntry.Key;
			FLSATTrimTimesMap& TrimTimesMapRef = SoundWaveEntry.Value;

			// Generate fragmented TrimTimesArray based on the sound wave
			TArray<FLSATTrimTimes> TrimTimesArray;
			TrimTimesMapRef.GetKeys(/*out*/TrimTimesArray);
			GetFragmentedTrimTimes(/*out*/TrimTimesArray, SoundWave);

			// Log the newly created TrimTimes
			const FFrameRate TickResolution = GetTickResolution(TrimTimesMapRef.GetFirstAudioSection());
			for (const FLSATTrimTimes& It : TrimTimesArray)
			{
				UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Created new TrimTimes: [%d ms (%d frames) - %d ms (%d frames)]"),
				       __FUNCTION__, It.GetSoundTrimStartMs(), It.GetSoundTrimStartFrame(TickResolution), It.GetSoundTrimEndMs(), It.GetSoundTrimEndFrame(TickResolution));
			}

			// Replace the TrimTimesMap with the new fragmented TrimTimes
			TrimTimesMapRef.RebuildTrimTimesMapWithProcessor([&](UMovieSceneAudioSection* AudioSection, const FLSATTrimTimes& TrimTimes, FLSATSectionsContainer& OutAllNewSections)
			{
				CreateAudioSectionsByTrimTimes(AudioSection, TrimTimesArray, OutAllNewSections, TrimTimes);
			});
		}
		break;

	default:
		ensureMsgf(false, TEXT("ERROR: [%i] %hs:\nUnhandled PolicySegmentsReuse value!"), __LINE__, __FUNCTION__);
	}
}

/*********************************************************************************************
 * Main Flow
 ********************************************************************************************* */

// Exports a sound wave to a WAV file
FString ULSATUtilsLibrary::ExportSoundWaveToWav(USoundWave* SoundWave)
{
	if (!SoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Invalid SoundWave asset."), __FUNCTION__);
		return FString();
	}

	const FString PackagePath = SoundWave->GetPathName();
	const FString RelativePath = FPackageName::LongPackageNameToFilename(PackagePath, TEXT(""));
	const FString FullPath = FPaths::ChangeExtension(RelativePath, TEXT("wav"));
	const FString ExportPath = FPaths::ConvertRelativePathToFull(FullPath);

	// Export the sound wave to the WAV file
	UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
	ExportTask->Object = SoundWave;
	ExportTask->Exporter = UExporter::FindExporter(SoundWave, TEXT("wav"));
	ExportTask->Filename = ExportPath;
	ExportTask->bSelected = false;
	ExportTask->bReplaceIdentical = true;
	ExportTask->bPrompt = false;
	ExportTask->bUseFileArchive = false;
	ExportTask->bWriteEmptyFiles = false;
	ExportTask->bAutomated = true;

	const bool bSuccess = UExporter::RunAssetExportTask(ExportTask) == 1;

	if (bSuccess)
	{
		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Successfully exported SoundWave to: %s"), __FUNCTION__, *ExportPath);
		return ExportPath;
	}

	UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Failed to export SoundWave to: %s"), __FUNCTION__, *ExportPath);
	return FString();
}

// Trims an audio file to the specified start and end times
bool ULSATUtilsLibrary::TrimAudio(const FLSATTrimTimes& TrimTimes, const FString& InputPath, const FString& OutputPath)
{
	if (!TrimTimes.IsValid())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Invalid TrimTimes."), __FUNCTION__);
		return false;
	}

	int32 ReturnCode;
	FString Output;
	FString Errors;

	const float StartTimeSec = TrimTimes.GetSoundTrimStartSeconds();
	const float EndTimeSec = TrimTimes.GetSoundTrimEndSeconds();
	const FString& FfmpegPath = FLevelSequencerAudioTrimmerEdModule::GetFfmpegPath();
	const FString CommandLineArgs = FString::Printf(TEXT("-i \"%s\" -ss %.2f -to %.2f -c copy \"%s\" -y"), *InputPath, StartTimeSec, EndTimeSec, *OutputPath);

	// Execute the ffmpeg process
	FPlatformProcess::ExecProcess(*FfmpegPath, *CommandLineArgs, &ReturnCode, &Output, &Errors);

	if (ReturnCode != 0)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: FFMPEG failed to trim audio. Error: %s"), __FUNCTION__, *Errors);
		return false;
	}

	const float PrevSizeMB = IFileManager::Get().FileSize(*InputPath) / (1024.f * 1024.f);
	const float NewSizeMB = IFileManager::Get().FileSize(*OutputPath) / (1024.f * 1024.f);

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Trimmed audio stats: Previous Size: %.2f MB, New Size: %.2f MB"), __FUNCTION__, PrevSizeMB, NewSizeMB);

	return true;
}

// Reimports an audio file into the original sound wave asset in Unreal Engine
bool ULSATUtilsLibrary::ReimportAudioToUnreal(USoundWave* OriginalSoundWave, const FString& TrimmedAudioFilePath)
{
	if (!OriginalSoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Original SoundWave is null."), __FUNCTION__);
		return false;
	}

	if (!FPaths::FileExists(TrimmedAudioFilePath))
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Trimmed audio file does not exist: %s"), __FUNCTION__, *TrimmedAudioFilePath);
		return false;
	}

	// Update the reimport path
	TArray<FString> Filenames;
	Filenames.Add(TrimmedAudioFilePath);
	FReimportManager::Instance()->UpdateReimportPaths(OriginalSoundWave, Filenames);

	// Reimport the asset
	const bool bReimportSuccess = FReimportManager::Instance()->Reimport(OriginalSoundWave, false, false);
	if (!bReimportSuccess)
	{
		UE_LOG(LogAudioTrimmer, Error, TEXT("%hs: Failed to reimport asset: %s"), __FUNCTION__, *OriginalSoundWave->GetName());
		return false;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Successfully reimported asset: %s with new source: %s"), __FUNCTION__, *OriginalSoundWave->GetName(), *TrimmedAudioFilePath);
	return true;
}

// Resets the start frame offset of an audio section to 0 and sets a new sound wave
void ULSATUtilsLibrary::ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection, USoundWave* OptionalNewSound/* = nullptr*/)
{
	if (!ensureMsgf(AudioSection, TEXT("ASSERT: [%i] %hs:\n'AudioSection' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	if (OptionalNewSound)
	{
		AudioSection->SetSound(OptionalNewSound);
	}

	AudioSection->SetStartOffset(0);
	AudioSection->SetLooping(false);

	// Mark as modified
	AudioSection->MarkAsChanged();
	const UMovieScene* MovieScene = AudioSection->GetTypedOuter<UMovieScene>();
	if (MovieScene)
	{
		MovieScene->MarkPackageDirty();
	}
}

// Deletes a temporary WAV file from the file system
bool ULSATUtilsLibrary::DeleteTempWavFile(const FString& FilePath)
{
	if (FPaths::FileExists(FilePath))
	{
		if (IFileManager::Get().Delete(*FilePath))
		{
			UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Successfully deleted temporary file: %s"), __FUNCTION__, *FilePath);
			return true;
		}

		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Failed to delete temporary file: %s"), __FUNCTION__, *FilePath);
		return false;
	}
	return true; // File doesn't exist, so consider it successfully "deleted"
}

/*********************************************************************************************
 * Helpers
 ********************************************************************************************* */

// Duplicates the given SoundWave asset, incrementing an index to its name
USoundWave* ULSATUtilsLibrary::DuplicateSoundWave(USoundWave* OriginalSoundWave, int32 DuplicateIndex/* = 1*/)
{
	checkf(OriginalSoundWave, TEXT("ERROR: [%i] %hs:\n'OriginalSoundWave' is null!"), __LINE__, __FUNCTION__);

	// Generate a new name with incremented index (e.g. SoundWave -> SoundWave1 or SoundWave1 -> SoundWave2)
	const FString NewObjectName = [&]()-> FString
	{
		const FString& Name = OriginalSoundWave->GetName();
		const int32 Index = Name.FindLastCharByPredicate([](TCHAR Char) { return !FChar::IsDigit(Char); });
		const int32 NewIndex = (Index + 1 < Name.Len()) ? FCString::Atoi(*Name.Mid(Index + 1)) + DuplicateIndex : DuplicateIndex;
		return FString::Printf(TEXT("%s%d"), *Name.Left(Index + 1), NewIndex);
	}();

	if (!ensureMsgf(OriginalSoundWave->GetName() != NewObjectName, TEXT("ASSERT: [%i] %hs:\n'NewObjectName' is the same as 'OriginalSoundWave' name!: %s"), __LINE__, __FUNCTION__, *NewObjectName))
	{
		return nullptr;
	}

	// Get the original package name and create a new package within the same directory
	const FString OriginalPackagePath = FPackageName::GetLongPackagePath(OriginalSoundWave->GetOutermost()->GetName());
	const FString NewPackageName = FString::Printf(TEXT("%s/%s"), *OriginalPackagePath, *NewObjectName);
	UPackage* DuplicatedPackage = CreatePackage(*NewPackageName);

	// Duplicate the sound wave
	USoundWave* DuplicatedSoundWave = Cast<USoundWave>(StaticDuplicateObject(OriginalSoundWave, DuplicatedPackage, *NewObjectName));
	checkf(DuplicatedSoundWave, TEXT("ERROR: [%i] %hs:\nFailed to duplicate SoundWave: %s"), __LINE__, __FUNCTION__, *OriginalSoundWave->GetName());

	// Complete the duplication process
	DuplicatedSoundWave->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DuplicatedSoundWave);

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Duplicated sound wave %s to %s"), __FUNCTION__, *OriginalSoundWave->GetName(), *NewObjectName);

	return DuplicatedSoundWave;
}

// Duplicates the given audio section in the specified start and end frames
UMovieSceneAudioSection* ULSATUtilsLibrary::DuplicateAudioSection(UMovieSceneAudioSection* OriginalAudioSection, FFrameNumber SectionStart, FFrameNumber SectionEnd, FFrameNumber SoundStartOffset)
{
	if (!ensureMsgf(OriginalAudioSection, TEXT("ASSERT: [%i] %hs:\n'OriginalAudioSection' is not valid!"), __LINE__, __FUNCTION__)
		|| !ensureMsgf(SectionStart < SectionEnd, TEXT("ASSERT: [%i] %hs:\n'StartFrame' %d is not less than 'EndFrame' %d!"), __LINE__, __FUNCTION__, SectionStart.Value, SectionEnd.Value))
	{
		return nullptr;
	}

	// Duplicate the original audio section
	UMovieSceneTrack* Track = CastChecked<UMovieSceneTrack>(OriginalAudioSection->GetOuter());
	UMovieSceneAudioSection* DuplicatedSection = DuplicateObject<UMovieSceneAudioSection>(OriginalAudioSection, Track);
	if (!DuplicatedSection)
	{
		UE_LOG(LogAudioTrimmer, Error, TEXT("%hs: Failed to duplicate audio section: %s"), __FUNCTION__, *OriginalAudioSection->GetName());
		return nullptr;
	}

	// Add the duplicated section to the track
	Track->AddSection(*DuplicatedSection);

	// Set the range for the duplicated section using the start and end frame numbers
	const TRange<FFrameNumber> NewSectionRange(SectionStart, SectionEnd);
	DuplicatedSection->SetRange(NewSectionRange);
	DuplicatedSection->SetStartOffset(SoundStartOffset);

	return DuplicatedSection;
}

// Retrieves all audio sections from the given level sequence
void ULSATUtilsLibrary::FindAudioSectionsInLevelSequence(TMap<USoundWave*, FLSATSectionsContainer>& OutMap, const ULevelSequence* InLevelSequence)
{
	if (!InLevelSequence)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Invalid LevelSequence."), __FUNCTION__);
		return;
	}

	if (!OutMap.IsEmpty())
	{
		OutMap.Empty();
	}

	for (UMovieSceneTrack* Track : InLevelSequence->GetMovieScene()->GetTracks())
	{
		const UMovieSceneAudioTrack* AudioTrack = Cast<UMovieSceneAudioTrack>(Track);
		if (!AudioTrack)
		{
			continue;
		}

		for (UMovieSceneSection* Section : AudioTrack->GetAllSections())
		{
			UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(Section);
			USoundWave* SoundWave = AudioSection ? Cast<USoundWave>(AudioSection->GetSound()) : nullptr;
			if (SoundWave)
			{
				OutMap.FindOrAdd(SoundWave).Add(AudioSection);
			}
		}
	}
}

// Finds all assets that directly reference the given sound wave
void ULSATUtilsLibrary::FindAudioUsagesBySoundAsset(TArray<UObject*>& OutUsages, const USoundWave* InSound)
{
	if (!ensureMsgf(InSound, TEXT("ASSERT: [%i] %hs:\n'InSound' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	if (!OutUsages.IsEmpty())
	{
		OutUsages.Empty();
	}

	TArray<FName> AllReferences;
	const IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	AssetRegistry.GetReferencers(*InSound->GetOuter()->GetPathName(), AllReferences);

	for (const FName It : AllReferences)
	{
		TArray<FAssetData> NewAssets;
		AssetRegistry.GetAssetsByPackageName(It, NewAssets);
		for (const FAssetData& AssetData : NewAssets)
		{
			if (UObject* ReferencingObject = AssetData.GetAsset())
			{
				OutUsages.AddUnique(ReferencingObject);
			}
		}
	}
}

// Calculates the start and end times in milliseconds for trimming multiple audio sections
void ULSATUtilsLibrary::CalculateTrimTimesInAllSections(FLSATTrimTimesMap& OutTrimTimesMap, const FLSATSectionsContainer& AudioSections)
{
	for (UMovieSceneAudioSection* AudioSection : AudioSections)
	{
		if (!AudioSection)
		{
			continue;
		}

		FLSATTrimTimes TrimTimes(AudioSection);
		if (TrimTimes.IsValid())
		{
			OutTrimTimesMap.Add(MoveTemp(TrimTimes), AudioSection);
		}
	}
}

//Calculates the start and end times in milliseconds for trimming an audio section
FLSATTrimTimes ULSATUtilsLibrary::MakeTrimTimes(const UMovieSceneAudioSection* AudioSection)
{
	return FLSATTrimTimes(AudioSection);
}

// Splits the looping segments in the given trim times into multiple sections
void ULSATUtilsLibrary::SplitLoopingSection(FLSATSectionsContainer& OutNewSectionsContainer, UMovieSceneAudioSection* InAudioSection, const FLSATTrimTimes& TrimTimes)
{
	const FFrameRate TickResolution = GetTickResolution(InAudioSection);
	if (!ensureMsgf(TickResolution.IsValid(), TEXT("ASSERT: [%i] %hs:\n'TickResolution' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Splitting looping sections for %s"), __FUNCTION__, *TrimTimes.ToString(TickResolution));

	if (!ensureMsgf(TrimTimes.IsValid(), TEXT("ASSERT: [%i] %hs:\n'TrimTimes' is not valid"), __LINE__, __FUNCTION__)
		|| !ensureMsgf(InAudioSection, TEXT("ASSERT: [%i] %hs:\n'InAudioSection' is null!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	const int32 TotalSoundDurationMs = TrimTimes.GetSoundTotalDurationMs();
	const int32 SectionStartMs = GetSectionInclusiveStartTimeMs(InAudioSection);
	const int32 SectionEndMs = GetSectionExclusiveEndTimeMs(InAudioSection);

	OutNewSectionsContainer.Add(InAudioSection);

	int32 CurrentStartTimeMs = SectionStartMs;
	int32 SplitDurationMs = TotalSoundDurationMs - TrimTimes.GetSoundTrimStartMs();

	// Continue splitting until the end time is reached
	while (CurrentStartTimeMs < SectionEndMs - SplitDurationMs)
	{
		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: CurrentStartTimeMs: %d, TrimTimes.GetSoundTrimEndMs(): %d"), __FUNCTION__, CurrentStartTimeMs, TrimTimes.GetSoundTrimEndMs());

		// Calculate the next split end time
		const int32 NextEndTimeMs = FMath::Min(CurrentStartTimeMs + SplitDurationMs, SectionEndMs);
		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: NextEndTimeMs: %d"), __FUNCTION__, NextEndTimeMs);

		// Convert the current start time to frame time for splitting
		const FFrameNumber SplitFrame = TickResolution.AsFrameNumber(NextEndTimeMs / 1000.f);
		const FQualifiedFrameTime SplitTime(SplitFrame, TickResolution);
		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Attempting to split at time: %d ms, which is frame: %d"), __FUNCTION__, CurrentStartTimeMs, SplitTime.Time.GetFrame().Value);

		// Check if the section contains the split time
		if (!InAudioSection->GetRange().Contains(SplitTime.Time.GetFrame()))
		{
			UE_LOG(LogAudioTrimmer, Error, TEXT("%hs: ERROR: Section '%s' does not contain the split time: %d | %s"), __FUNCTION__, *InAudioSection->GetName(), SplitTime.Time.GetFrame().Value, *TrimTimes.ToString(TickResolution));
			return;
		}

		// Perform the split
		constexpr bool bDeleteKeysWhenTrimming = false;
		UMovieSceneAudioSection* NewSection = Cast<UMovieSceneAudioSection>(InAudioSection->SplitSection(SplitTime, bDeleteKeysWhenTrimming));
		if (!NewSection)
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("%hs: Failed to split section: %s"), __FUNCTION__, *InAudioSection->GetName());
			return;
		}

		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Created new section: %s with range: [%d, %d]"), __FUNCTION__, *NewSection->GetName(), NewSection->GetInclusiveStartFrame().Value, NewSection->GetExclusiveEndFrame().Value);

		// Reset the newly created section
		ResetTrimmedAudioSection(NewSection);
		OutNewSectionsContainer.Add(NewSection);

		// Override the split duration, so only first time respects start time offset and the rest are full duration 
		SplitDurationMs = TotalSoundDurationMs;

		// Move to the next split point
		InAudioSection = NewSection;
		CurrentStartTimeMs = NextEndTimeMs;
		UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Next CurrentStartTimeMs: %d"), __FUNCTION__, CurrentStartTimeMs);
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("%hs: Splitting complete, split into %d new sections."), __FUNCTION__, OutNewSectionsContainer.Num());
}

// Returns the actual start time of the audio section in the level Sequence in milliseconds
int32 ULSATUtilsLibrary::GetSectionInclusiveStartTimeMs(const UMovieSceneSection* InSection)
{
	const FFrameRate TickResolution = GetTickResolution(InSection);
	if (!TickResolution.IsValid())
	{
		return INDEX_NONE;
	}

	const FFrameNumber SectionStartFrame = InSection->GetInclusiveStartFrame();
	return ConvertFrameToMs(SectionStartFrame, TickResolution);
}

// Returns the actual end time of the audio section in the level Sequence in milliseconds
int32 ULSATUtilsLibrary::GetSectionExclusiveEndTimeMs(const UMovieSceneSection* InSection)
{
	const FFrameRate TickResolution = GetTickResolution(InSection);
	if (!TickResolution.IsValid())
	{
		return INDEX_NONE;
	}

	const FFrameNumber SectionEndFrame = InSection->GetExclusiveEndFrame();
	return ConvertFrameToMs(SectionEndFrame, TickResolution);
}

// Converts seconds to frames based on the frame rate, or -1 if cannot convert
int32 ULSATUtilsLibrary::ConvertMsToFrame(int32 InMilliseconds, const FFrameRate& TickResolution)
{
	// Convert time in seconds to frame time based on tick resolution
	const float InSec = static_cast<float>(InMilliseconds) / 1000.f;
	const float FrameNumber = static_cast<float>(TickResolution.AsFrameTime(InSec).GetFrame().Value);
	const float Frame = FrameNumber / 1000.f;

	return FrameNumber >= 0.f ? FMath::RoundToInt(Frame) : INDEX_NONE;
}

FFrameNumber ULSATUtilsLibrary::ConvertMsToFrameNumber(int32 InMilliseconds, const FFrameRate& TickResolution)
{
	return ConvertMsToFrame(InMilliseconds, TickResolution) * 1000;
}

// Converts frames to milliseconds based on the frame rate, or -1 if cannot convert
int32 ULSATUtilsLibrary::ConvertFrameToMs(const FFrameNumber& InFrame, const FFrameRate& TickResolution)
{
	if (!TickResolution.IsValid())
	{
		return INDEX_NONE;
	}

	const double InSec = TickResolution.AsSeconds(InFrame);
	const double InMs = InSec * 1000.0;
	return FMath::RoundToInt(InMs);
}

// Returns the tick resolution of the level sequence
FFrameRate ULSATUtilsLibrary::GetTickResolution(const UMovieSceneSection* InSection)
{
	FFrameRate TickResolution(0, 0);
	if (const ULevelSequence* InLevelSequence = GetLevelSequence(InSection))
	{
		TickResolution = InLevelSequence->GetMovieScene()->GetTickResolution();
	}
	return TickResolution;
}

// Returns the Level Sequence of the given section
ULevelSequence* ULSATUtilsLibrary::GetLevelSequence(const UMovieSceneSection* InSection)
{
	return InSection ? InSection->GetTypedOuter<ULevelSequence>() : nullptr;
}

// Splits the given trim times into smaller, non-overlapping parts that can be reused
void ULSATUtilsLibrary::GetFragmentedTrimTimes(TArray<FLSATTrimTimes>& InOutTrimTimes, USoundWave* SoundWave)
{
	TArray<FLSATTrimTimes> NewTrimTimesArray;

	// Collect all start and end times as split points
	TSet<int32> SplitPointsMs;
	for (const FLSATTrimTimes& It : InOutTrimTimes)
	{
		SplitPointsMs.Add(It.GetSoundTrimStartMs());
		SplitPointsMs.Add(It.GetSoundTrimEndMs());
	}

	// Convert the split points to a sorted array
	TArray<int32> SortedSplitPointsMs = SplitPointsMs.Array();
	SortedSplitPointsMs.Sort();

	// Create new trim times based on the split points
	const int32 MinDurationMs = ULSATSettings::Get().MinDifferenceMs;

	for (int32 i = 0; i < SortedSplitPointsMs.Num() - 1; ++i)
	{
		const int32 StartMs = SortedSplitPointsMs[i];
		const int32 EndMs = SortedSplitPointsMs[i + 1];

		const int32 SegmentDurationMs = EndMs - StartMs;

		if (SegmentDurationMs < MinDurationMs)
		{
			continue; // Skip segments shorter than MinDifferenceMs
		}

		FLSATTrimTimes NewTrimTimes(StartMs, EndMs, SoundWave);
		NewTrimTimesArray.Add(MoveTemp(NewTrimTimes));
	}

	InOutTrimTimes = NewTrimTimesArray;
}

// Creates new audio sections by duplicating the original section based on the provided trim times, adjusting start and end times to fit within the valid range
void ULSATUtilsLibrary::CreateAudioSectionsByTrimTimes(UMovieSceneAudioSection* OriginalAudioSection, const TArray<FLSATTrimTimes>& InTrimTimes, FLSATSectionsContainer& OutAllNewSections, const FLSATTrimTimes& InRange)
{
	const FFrameRate TickResolution = GetTickResolution(OriginalAudioSection);
	if (!ensureMsgf(TickResolution.IsValid(), TEXT("ASSERT: [%i] %hs:\n'TickResolution' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	bool bCreatedAnySection = false;
	const int32 SectionStartMs = GetSectionInclusiveStartTimeMs(OriginalAudioSection);
	UMovieSceneTrack* Track = CastChecked<UMovieSceneTrack>(OriginalAudioSection->GetOuter());

	// Loop through each fragment in the range and adjust its timing relative to the sequence
	for (const FLSATTrimTimes& NewTrimTime : InTrimTimes)
	{
		const int32 GetSoundTrimStartMs = SectionStartMs + (NewTrimTime.GetSoundTrimStartMs() - InRange.GetSoundTrimStartMs());
		const int32 SoundTrimEndMs = SectionStartMs + (NewTrimTime.GetSoundTrimEndMs() - InRange.GetSoundTrimStartMs());
		const FLSATTrimTimes FragmentedTrimTimes(GetSoundTrimStartMs, SoundTrimEndMs, NewTrimTime.GetSoundWave());

		if (!FragmentedTrimTimes.IsWithinSectionBounds(OriginalAudioSection)
			|| !NewTrimTime.IsWithinTrimBounds(InRange))
		{
			continue;
		}

		const FFrameNumber SectionStartFrame = ConvertMsToFrameNumber(FragmentedTrimTimes.GetSoundTrimStartMs(), TickResolution);
		const FFrameNumber SectionEndFrame = ConvertMsToFrameNumber(FragmentedTrimTimes.GetSoundTrimEndMs(), TickResolution);
		const FFrameNumber SoundOffsetFrame = ConvertMsToFrameNumber(NewTrimTime.GetSoundTrimStartMs(), TickResolution);

		UMovieSceneAudioSection* NewSection = DuplicateAudioSection(OriginalAudioSection, SectionStartFrame, SectionEndFrame, SoundOffsetFrame);
		if (!ensureMsgf(NewSection, TEXT("ASSERT: [%i] %hs:\n'NewSection' failed to duplicate | %s!"), __LINE__, __FUNCTION__, *NewTrimTime.ToString(TickResolution)))
		{
			continue;
		}

		OutAllNewSections.Add(NewSection);

		bCreatedAnySection = true;
	}

	if (bCreatedAnySection)
	{
		Track->RemoveSection(*OriginalAudioSection);
	}
}
