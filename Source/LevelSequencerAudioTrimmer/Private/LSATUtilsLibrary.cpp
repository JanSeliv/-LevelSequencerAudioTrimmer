// Copyright (c) Yevhenii Selivanov

#include "LSATUtilsLibrary.h"
//---
#include "Data/LSATTrimTimesData.h"
//---
#include "AssetExportTask.h"
#include "AssetToolsModule.h"
#include "LevelSequence.h"
#include "LSATSettings.h"
#include "LevelSequencerAudioTrimmerEdModule.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "ObjectTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Exporters/Exporter.h"
#include "Factories/ReimportSoundFactory.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sections/MovieSceneSubSection.h"
#include "Sound/SampleBufferIO.h"
#include "Sound/SoundWave.h"
#include "Tests/AutomationEditorCommon.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "UObject/SavePackage.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LSATUtilsLibrary)

// Entry method to run the main flow of trimming all audio assets for the given level sequence
void ULSATUtilsLibrary::RunLevelSequenceAudioTrimmer(const TArray<ULevelSequence*>& LevelSequences)
{
	/*********************************************************************************************
	 * Preprocessing: Prepares the `TrimTimesMultiMap` map that combines sound waves with their corresponding trim times.
	 *********************************************************************************************
	 * 1. HandleSoundsInRequestedLevelSequence ➔ Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence.
	 * 2. HandleSoundsInOtherSequences ➔ Handles those sounds from original Level Sequence that are used at the same time in other Level Sequences.
	 * 3. HandlePolicyLoopingSounds ➔ Handles the policy for looping sounds based on the settings, e.g: skipping all looping sounds.
	 * 4. HandlePolicySoundsOutsideSequences ➔ Handle sound waves that are used outside of level sequences like in the world or blueprints.
	 ********************************************************************************************* */

	FLSATTrimTimesMultiMap TrimTimesMultiMap;

	for (const ULevelSequence* LevelSequence : LevelSequences)
	{
		HandleSoundsInRequestedLevelSequence(/*out*/TrimTimesMultiMap, LevelSequence);
		if (TrimTimesMultiMap.IsEmpty())
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("No valid trim times found in the level sequence."));
			return;
		}

		HandleSoundsInOtherSequences(/*InOut*/TrimTimesMultiMap);
		HandlePolicyLoopingSounds(/*InOut*/TrimTimesMultiMap);
		HandlePolicySoundsOutsideSequences(/*InOut*/TrimTimesMultiMap);
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d unique sound waves with valid trim times."), TrimTimesMultiMap.Num());

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
	 * 1. DuplicateSoundWave ➔ Optional, duplicates sound wave asset if needed.
	 * 2. ExportSoundWaveToWav ➔ Convert sound wave into a WAV file.
	 * 3. TrimAudio ➔ Apply trimming to the WAV file.
	 * 4. ReimportAudioToUnreal ➔ Load the trimmed WAV file back into the engine.
	 * 5. ResetTrimmedAudioSection ➔ Update audio section with the new sound.
	 * 6. DeleteTempWavFile ➔ Remove the temporary WAV file.
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
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Skipping processing for sound wave %s due to different trim times."), *GetNameSafe(OriginalSoundWave));
			continue;
		}

		int32 GroupIndex = 0;
		for (const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& InnerIt : InnerMap)
		{
			const FLSATTrimTimes& TrimTimes = InnerIt.Key;
			const FLSATSectionsContainer& Sections = InnerIt.Value;
			USoundWave* TrimmedSoundWave = OriginalSoundWave;

			if (TrimTimes.IsUsageSimilarToTotalDuration())
			{
				UE_LOG(LogAudioTrimmer, Log, TEXT("Skipping export for audio %s as there is almost no difference between total duration and usage duration"), *GetNameSafe(TrimTimes.SoundWave));
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
					UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to export %s"), *TrimmedSoundWave->GetName());
					continue;
				}

				// Perform the audio trimming
				const FString TrimmedAudioPath = FPaths::ChangeExtension(ExportPath, TEXT("_trimmed.wav"));
				const bool bSuccessTrim = TrimAudio(TrimTimes, ExportPath, TrimmedAudioPath);
				if (!bSuccessTrim)
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("Trimming audio failed for %s"), *TrimmedSoundWave->GetName());
					continue;
				}

				// Reimport the trimmed audio back into Unreal Engine
				const bool bSuccessReimport = ReimportAudioToUnreal(TrimmedSoundWave, TrimmedAudioPath);
				if (!bSuccessReimport)
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("Reimporting trimmed audio failed for %s"), *TrimmedSoundWave->GetName());
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

	UE_LOG(LogAudioTrimmer, Log, TEXT("Processing complete."));
}

/*********************************************************************************************
 * Preprocessing
 ********************************************************************************************* */

// Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence
void ULSATUtilsLibrary::HandleSoundsInRequestedLevelSequence(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap, const ULevelSequence* LevelSequence)
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
		UE_LOG(LogAudioTrimmer, Warning, TEXT("No audio sections found in the level sequence."));
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d unique sound waves in the main sequence."), MainAudioSectionsMap.Num());

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
void ULSATUtilsLibrary::HandleSoundsInOtherSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	for (TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& ItRef : InOutTrimTimesMultiMap)
	{
		const USoundWave* OriginalSoundWave = ItRef.Key;
		if (!OriginalSoundWave)
		{
			continue;
		}

		// Get first level sequence where the sound wave is used as iterator from the map
		const ULevelSequence* OriginalLevelSequence = ItRef.Value.GetFirstLevelSequence();
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

			UE_LOG(LogAudioTrimmer, Log, TEXT("Found sound wave '%s' usage in other level sequence: %s, its sections will be processed as well"), *OriginalSoundWave->GetName(), *OtherLevelSequence->GetName());

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

// Handles the policy for looping sounds based on the settings, e.g: skipping all looping sounds
void ULSATUtilsLibrary::HandlePolicyLoopingSounds(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	TArray<USoundWave*> LoopingSounds;
	InOutTrimTimesMultiMap.GetLoopingSounds(LoopingSounds);

	if (LoopingSounds.IsEmpty())
	{
		return;
	}

	switch (ULSATSettings::Get().PolicyLoopingSounds)
	{
	case ELSATPolicyLoopingSounds::SkipAll:
		// Looping sounds should not be processed at all for this and all other audio tracks that use the same sound wave
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
			FLSATTrimTimesMap& TrimTimesMap = InOutTrimTimesMultiMap.FindOrAdd(LoopingSound);
			USoundWave* DuplicatedSound = nullptr;

			for (TTuple<FLSATTrimTimes, FLSATSectionsContainer>& ItRef : TrimTimesMap)
			{
				FLSATTrimTimes& TrimTimes = ItRef.Key;
				FLSATSectionsContainer& SectionsContainer = ItRef.Value;

				if (TrimTimes.IsLooping())
				{
					// Skip the looping sections
					continue;
				}

				// Duplicate the sound wave for non-looping sections if not already done
				if (!DuplicatedSound)
				{
					DuplicatedSound = DuplicateSoundWave(LoopingSound);
				}

				// Assign the duplicated sound wave to all audio sections in this trim time
				SectionsContainer.SetSound(DuplicatedSound);

				// Move the non-looping trim times to the duplicated sound wave in the map
				InOutTrimTimesMultiMap.FindOrAdd(DuplicatedSound).Add(TrimTimes, SectionsContainer);
			}

			// Remove looping sound wave from the map, all other usages were duplicated into separate sound wave asset
			if (TrimTimesMap.IsEmpty()
				|| DuplicatedSound)
			{
				InOutTrimTimesMultiMap.Remove(LoopingSound);
			}
		}
		break;

	default:
		ensureMsgf(false, TEXT("ERROR: [%i] %hs:\nUnhandled PolicyLoopingSounds value!"), __LINE__, __FUNCTION__);
	}
}

// Main goal of this function is to handle those sounds that are used outside of level sequences like in the world or blueprints
// Handles the policy for sounds used outside of level sequences, e.g., skipping or duplicating them.
void ULSATUtilsLibrary::HandlePolicySoundsOutsideSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap)
{
	TArray<USoundWave*> SoundsOutsideSequences;
	for (const TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& It : InOutTrimTimesMultiMap)
	{
		if (!It.Key)
		{
			continue;
		}

		// Find other usages of the sound wave outside the level sequences.
		TArray<UObject*> OutUsages;
		FindAudioUsagesBySoundAsset(OutUsages, It.Key);

		const bool bHasExternalUsages = OutUsages.ContainsByPredicate([](const UObject* Usage) { return Usage && !Usage->IsA<ULevelSequence>(); });
		if (bHasExternalUsages)
		{
			SoundsOutsideSequences.Add(It.Key);
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Sound wave '%s' is used outside of level sequences by different assets (like in the world or blueprints)"
				       ", `Sounds Outside Sequences` Policy will be applied"), *It.Key->GetName());
		}
	}

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

/*********************************************************************************************
 * Main Flow
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

	UE_LOG(LogAudioTrimmer, Log, TEXT("Duplicated sound wave %s to %s"), *OriginalSoundWave->GetName(), *NewObjectName);

	return DuplicatedSoundWave;
}

// Exports a sound wave to a WAV file
FString ULSATUtilsLibrary::ExportSoundWaveToWav(USoundWave* SoundWave)
{
	if (!SoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid SoundWave asset."));
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
		UE_LOG(LogAudioTrimmer, Log, TEXT("Successfully exported SoundWave to: %s"), *ExportPath);
		return ExportPath;
	}

	UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to export SoundWave to: %s"), *ExportPath);
	return FString();
}

// Trims an audio file to the specified start and end times
bool ULSATUtilsLibrary::TrimAudio(const FLSATTrimTimes& TrimTimes, const FString& InputPath, const FString& OutputPath)
{
	if (!TrimTimes.IsValid())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid TrimTimes."));
		return false;
	}

	int32 ReturnCode;
	FString Output;
	FString Errors;

	const float StartTimeSec = TrimTimes.StartTimeMs / 1000.0f;
	const float EndTimeSec = TrimTimes.EndTimeMs / 1000.0f;
	const FString& FfmpegPath = FLevelSequencerAudioTrimmerEdModule::GetFfmpegPath();
	const FString CommandLineArgs = FString::Printf(TEXT("-i \"%s\" -ss %.2f -to %.2f -c copy \"%s\" -y"), *InputPath, StartTimeSec, EndTimeSec, *OutputPath);

	// Execute the ffmpeg process
	FPlatformProcess::ExecProcess(*FfmpegPath, *CommandLineArgs, &ReturnCode, &Output, &Errors);

	if (ReturnCode != 0)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("FFMPEG failed to trim audio. Error: %s"), *Errors);
		return false;
	}

	const float PrevSizeMB = IFileManager::Get().FileSize(*InputPath) / (1024.f * 1024.f);
	const float NewSizeMB = IFileManager::Get().FileSize(*OutputPath) / (1024.f * 1024.f);

	UE_LOG(LogAudioTrimmer, Log, TEXT("Trimmed audio stats: Previous Size: %.2f MB, New Size: %.2f MB"), PrevSizeMB, NewSizeMB);

	return true;
}

// Reimports an audio file into the original sound wave asset in Unreal Engine
bool ULSATUtilsLibrary::ReimportAudioToUnreal(USoundWave* OriginalSoundWave, const FString& TrimmedAudioFilePath)
{
	if (!OriginalSoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Original SoundWave is null."));
		return false;
	}

	if (!FPaths::FileExists(TrimmedAudioFilePath))
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Trimmed audio file does not exist: %s"), *TrimmedAudioFilePath);
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
		UE_LOG(LogAudioTrimmer, Error, TEXT("Failed to reimport asset: %s"), *OriginalSoundWave->GetName());
		return false;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Successfully reimported asset: %s with new source: %s"), *OriginalSoundWave->GetName(), *TrimmedAudioFilePath);
	return true;
}

// Resets the start frame offset of an audio section to 0 and sets a new sound wave
void ULSATUtilsLibrary::ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection, USoundWave* NewSound)
{
	if (!ensureMsgf(AudioSection, TEXT("ASSERT: [%i] %hs:\n'AudioSection' is not valid!"), __LINE__, __FUNCTION__)
		|| !ensureMsgf(NewSound, TEXT("ASSERT: [%i] %hs:\n'NewSound' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	AudioSection->SetSound(NewSound);
	AudioSection->SetStartOffset(0);
	AudioSection->SetLooping(false);

	// Mark as modified
	AudioSection->MarkAsChanged();
	const UMovieScene* MovieScene = AudioSection->GetTypedOuter<UMovieScene>();
	if (MovieScene)
	{
		MovieScene->MarkPackageDirty();
	}

	// Log the operation
	UE_LOG(LogAudioTrimmer, Log, TEXT("Reset Start Frame Offset and adjusted duration for section using sound: %s"), *GetNameSafe(NewSound));
}

// Deletes a temporary WAV file from the file system
bool ULSATUtilsLibrary::DeleteTempWavFile(const FString& FilePath)
{
	if (FPaths::FileExists(FilePath))
	{
		if (IFileManager::Get().Delete(*FilePath))
		{
			UE_LOG(LogAudioTrimmer, Log, TEXT("Successfully deleted temporary file: %s"), *FilePath);
			return true;
		}

		UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to delete temporary file: %s"), *FilePath);
		return false;
	}
	return true; // File doesn't exist, so consider it successfully "deleted"
}

/*********************************************************************************************
 * Utilities
 ********************************************************************************************* */

// Retrieves all audio sections from the given level sequence
void ULSATUtilsLibrary::FindAudioSectionsInLevelSequence(TMap<USoundWave*, FLSATSectionsContainer>& OutMap, const ULevelSequence* InLevelSequence)
{
	if (!InLevelSequence)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid LevelSequence."));
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

		FLSATTrimTimes TrimTimes = CalculateTrimTimesInSection(AudioSection);
		if (TrimTimes.IsValid())
		{
			OutTrimTimesMap.Add(TrimTimes, AudioSection);
		}
	}
}

//Calculates the start and end times in milliseconds for trimming an audio section
FLSATTrimTimes ULSATUtilsLibrary::CalculateTrimTimesInSection(UMovieSceneAudioSection* AudioSection)
{
	const ULevelSequence* LevelSequence = AudioSection ? AudioSection->GetTypedOuter<ULevelSequence>() : nullptr;
	if (!LevelSequence)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid LevelSequence or AudioSection."));
		return FLSATTrimTimes::Invalid;
	}

	const FFrameRate TickResolution = LevelSequence->GetMovieScene()->GetTickResolution();

	// Get the audio start offset in frames (relative to the audio asset)
	const int32 AudioStartOffsetFrames = AudioSection->GetStartOffset().Value;
	const float AudioStartOffsetSeconds = AudioStartOffsetFrames / TickResolution.AsDecimal();

	// Get the duration of the section on the track (in frames)
	const int32 SectionDurationFrames = (AudioSection->GetExclusiveEndFrame() - AudioSection->GetInclusiveStartFrame()).Value;
	const float SectionDurationSeconds = SectionDurationFrames / TickResolution.AsDecimal();

	// Calculate the effective end time within the audio asset
	const float AudioEndSeconds = AudioStartOffsetSeconds + SectionDurationSeconds;

	USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
	if (!ensureMsgf(SoundWave, TEXT("ASSERT: [%i] %hs:\n'SoundWave' is not valid!"), __LINE__, __FUNCTION__))
	{
		return FLSATTrimTimes::Invalid;
	}

	FLSATTrimTimes TrimTimes;
	TrimTimes.StartTimeMs = static_cast<int32>(AudioStartOffsetSeconds * 1000.0f);
	TrimTimes.EndTimeMs = static_cast<int32>(AudioEndSeconds * 1000.0f);
	TrimTimes.SoundWave = SoundWave;

	if (TrimTimes.IsLooping())
	{
		const int32 StartFrameIndex = AudioSection->GetInclusiveStartFrame().Value;
		const int32 EndFrameIndex = AudioSection->GetExclusiveEndFrame().Value;
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Audio section is looping and starts from the beginning. Level Sequence: %s, Audio Asset: %s, Section Range: %d - %d"),
		       *LevelSequence->GetName(), *SoundWave->GetName(),
		       FMath::RoundToInt(StartFrameIndex / 1000.f),
		       FMath::RoundToInt(EndFrameIndex / 1000.f));
	}

	// Log the start and end times in milliseconds, section duration, and percentage used
	UE_LOG(LogAudioTrimmer, Log, TEXT("Audio: %s, Used from %.2f seconds to %.2f seconds (Duration: %.2f seconds), Percentage Used: %.2f%%"),
	       *SoundWave->GetName(), AudioStartOffsetSeconds, AudioEndSeconds, AudioEndSeconds - AudioStartOffsetSeconds,
	       ((AudioEndSeconds - AudioStartOffsetSeconds) / SoundWave->Duration) * 100.0f);

	return TrimTimes;
}
