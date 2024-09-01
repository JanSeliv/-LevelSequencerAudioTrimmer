// Copyright (c) Yevhenii Selivanov

#include "AudioTrimmerUtilsLibrary.h"
//---
#include "LevelSequencerAudioTypes.h"
//---
#include "AssetExportTask.h"
#include "AssetToolsModule.h"
#include "LevelSequence.h"
#include "LevelSequencerAudioSettings.h"
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
#include UE_INLINE_GENERATED_CPP_BY_NAME(AudioTrimmerUtilsLibrary)

// Entry method to run the main flow of trimming all audio assets for the given level sequence
void UAudioTrimmerUtilsLibrary::RunLevelSequenceAudioTrimmer(const ULevelSequence* LevelSequence)
{
	/*********************************************************************************************
	 * Preprocessing: Prepares the `SoundsTrimTimesMap` map that combines sound waves with their corresponding trim times.
	 *********************************************************************************************
	 * [Flow]
	 * 1. HandleSoundsInRequestedLevelSequence ➔ Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence.
	 * 2. HandleSoundsInOtherSequences ➔ Handles those sounds from original Level Sequence that are used at the same time in other Level Sequences.
	 * 3. HandleSoundsOutsideSequences ➔ Handle sound waves that are used outside of level sequences like in the world or blueprints.
	 ********************************************************************************************* */

	// Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence
	FSoundsTrimTimesMap SoundsTrimTimesMap;
	HandleSoundsInRequestedLevelSequence(/*out*/SoundsTrimTimesMap, LevelSequence);

	if (SoundsTrimTimesMap.IsEmpty())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("No valid trim times found in the level sequence."));
		return;
	}

	// Handles those sounds from original Level Sequence that are used at the same time in other Level Sequences
	HandleSoundsInOtherSequences(/*InOut*/SoundsTrimTimesMap);

	// Handle sound waves that are used outside of level sequences like in the world or blueprints
	HandleSoundsOutsideSequences(/*InOut*/SoundsTrimTimesMap);

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d unique sound waves with valid trim times."), SoundsTrimTimesMap.Num());

	/*********************************************************************************************
	 * Iteration
	 *********************************************************************************************
	 * 
	 * [Example Data] - Let's assume we have the following sounds and audio sections in the level sequence:
	 * - SW_Ball is used twice: AudioSection0[15-30], AudioSection1[15-30]
	 * - SW_Step is used three times: AudioSection2[7-10], AudioSection3[7-10], AudioSection4[18-25]
	 * 
	 * [SoundsTrimTimesMap] - Illustrates how Example Data is iterated and processed:
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
	 * 1. DuplicateSoundWave ➔ Duplicate sound waves when needed.
	 * 2. ExportSoundWaveToWav ➔ Convert sound wave into a WAV file.
	 * 3. TrimAudio ➔ Apply trimming to the WAV file.
	 * 4. ReimportAudioToUnreal ➔ Load the trimmed WAV file back into the engine.
	 * 5. ResetTrimmedAudioSection ➔ Update audio section with the new sound.
	 * 6. DeleteTempWavFile ➔ Remove the temporary WAV file.
	 ******************************************************************************************* */

	for (const TTuple<TObjectPtr<USoundWave>, FTrimTimesMap>& OuterIt : SoundsTrimTimesMap)
	{
		USoundWave* const OriginalSoundWave = OuterIt.Key;
		const FTrimTimesMap& InnerMap = OuterIt.Value;

		int32 GroupIndex = 0;
		for (const TTuple<FTrimTimes, FAudioSectionsContainer>& InnerIt : InnerMap)
		{
			const FTrimTimes& TrimTimes = InnerIt.Key;
			const FAudioSectionsContainer& Sections = InnerIt.Value;
			USoundWave* TrimmedSoundWave = OriginalSoundWave;

			const bool bIsBeforeLastGroup = GroupIndex < InnerMap.Num() - 1;
			if (bIsBeforeLastGroup)
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
void UAudioTrimmerUtilsLibrary::HandleSoundsInRequestedLevelSequence(FSoundsTrimTimesMap& OutSoundsTrimTimesMap, const ULevelSequence* LevelSequence)
{
	if (!ensureMsgf(LevelSequence, TEXT("ASSERT: [%i] %hs:\n'LevelSequence' is not valid!"), __LINE__, __FUNCTION__))
	{
		return;
	}

	OutSoundsTrimTimesMap.Empty();

	// Retrieve audio sections mapped by SoundWave from the main Level Sequence
	TMap<USoundWave*, FAudioSectionsContainer> MainAudioSectionsMap;
	FindAudioSectionsInLevelSequence(MainAudioSectionsMap, LevelSequence);

	if (MainAudioSectionsMap.IsEmpty())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("No audio sections found in the level sequence."));
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d unique sound waves in the main sequence."), MainAudioSectionsMap.Num());

	for (const TTuple<USoundWave*, FAudioSectionsContainer>& It : MainAudioSectionsMap)
	{
		USoundWave* OriginalSoundWave = It.Key;
		const FAudioSectionsContainer& MainSections = It.Value;

		// Calculate and combine trim times for the main sequence
		FTrimTimesMap& TrimTimesMap = OutSoundsTrimTimesMap.FindOrAdd(OriginalSoundWave);
		CalculateTrimTimesInAllSections(TrimTimesMap, MainSections);
	}
}

// Handles those sounds from requested Level Sequence that are used at the same time in other Level Sequences
void UAudioTrimmerUtilsLibrary::HandleSoundsInOtherSequences(FSoundsTrimTimesMap& InOutSoundsTrimTimesMap)
{
	for (TTuple<TObjectPtr<USoundWave>, FTrimTimesMap>& ItRef : InOutSoundsTrimTimesMap)
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
			TMap<USoundWave*, FAudioSectionsContainer> OtherAudioSectionsMap;
			FindAudioSectionsInLevelSequence(OtherAudioSectionsMap, OtherLevelSequence);

			if (const FAudioSectionsContainer* OtherSectionsPtr = OtherAudioSectionsMap.Find(OriginalSoundWave))
			{
				// Calculate and combine trim times for the other level sequences
				CalculateTrimTimesInAllSections(ItRef.Value, *OtherSectionsPtr);
			}
		}
	}
}

// Main goal of this function is to handle those sounds that are used outside of level sequences like in the world or blueprints
void UAudioTrimmerUtilsLibrary::HandleSoundsOutsideSequences(FSoundsTrimTimesMap& InOutSoundsTrimTimesMap)
{
	for (TTuple<TObjectPtr<USoundWave>, FTrimTimesMap>& ItRef : InOutSoundsTrimTimesMap)
	{
		// Find other usages of the sound wave outside the level sequences
		TArray<UObject*> OutUsages;
		FindAudioUsagesBySoundAsset(OutUsages, ItRef.Key);

		const bool bHasExternalUsages = OutUsages.ContainsByPredicate([](const UObject* Usage) { return Usage && !Usage->IsA<ULevelSequence>(); });
		if (!bHasExternalUsages)
		{
			continue;
		}

		// display warning that sound is used by X assets, so we can't trim original sound asset, the Duplication Policy will be applied (
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Sound wave '%s' is used outside of level sequences by different assets (like in the world or blueprints)"
			       ", so we can't trim original sound asset. Duplication Policy will be applied (duplicate original sound or skip processing)"), *ItRef.Key->GetName());

		// We don't want to break other usages in levels and blueprints of this sound by reimporting with new timings
		// Therefore, duplicate it and replace the original sound wave with the duplicated one in given map
		USoundWave* DuplicatedSoundWave = DuplicateSoundWave(ItRef.Key);
		checkf(DuplicatedSoundWave, TEXT("ERROR: [%i] %hs:\n'DuplicatedSoundWave' failed to Duplicate!"), __LINE__, __FUNCTION__);
		ItRef.Key = DuplicatedSoundWave;
		for (TTuple<FTrimTimes, FAudioSectionsContainer>& InnerItRef : ItRef.Value)
		{
			InnerItRef.Key.SoundWave = DuplicatedSoundWave;
			for (UMovieSceneAudioSection* SectionIt : InnerItRef.Value)
			{
				checkf(SectionIt, TEXT("ERROR: [%i] %hs:\n'SectionIt' is null!"), __LINE__, __FUNCTION__);
				SectionIt->SetSound(DuplicatedSoundWave);
			}
		}
	}
}

/*********************************************************************************************
 * Main Flow
 ********************************************************************************************* */

// Duplicates the given SoundWave asset, incrementing an index to its name
USoundWave* UAudioTrimmerUtilsLibrary::DuplicateSoundWave(USoundWave* OriginalSoundWave, int32 DuplicateIndex/* = 1*/)
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

	if (!DuplicatedSoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to duplicate %s. Using original sound wave instead."), *OriginalSoundWave->GetName());
		return OriginalSoundWave;
	}

	// Complete the duplication process
	DuplicatedSoundWave->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DuplicatedSoundWave);

	UE_LOG(LogAudioTrimmer, Log, TEXT("Duplicated sound wave %s to %s"), *OriginalSoundWave->GetName(), *NewObjectName);

	return DuplicatedSoundWave;
}

// Exports a sound wave to a WAV file
FString UAudioTrimmerUtilsLibrary::ExportSoundWaveToWav(USoundWave* SoundWave)
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
bool UAudioTrimmerUtilsLibrary::TrimAudio(const FTrimTimes& TrimTimes, const FString& InputPath, const FString& OutputPath)
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
bool UAudioTrimmerUtilsLibrary::ReimportAudioToUnreal(USoundWave* OriginalSoundWave, const FString& TrimmedAudioFilePath)
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
void UAudioTrimmerUtilsLibrary::ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection, USoundWave* NewSound)
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
bool UAudioTrimmerUtilsLibrary::DeleteTempWavFile(const FString& FilePath)
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
void UAudioTrimmerUtilsLibrary::FindAudioSectionsInLevelSequence(TMap<USoundWave*, FAudioSectionsContainer>& OutMap, const ULevelSequence* InLevelSequence)
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
void UAudioTrimmerUtilsLibrary::FindAudioUsagesBySoundAsset(TArray<UObject*>& OutUsages, const USoundWave* InSound)
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
void UAudioTrimmerUtilsLibrary::CalculateTrimTimesInAllSections(FTrimTimesMap& OutTrimTimesMap, const FAudioSectionsContainer& AudioSections)
{
	for (UMovieSceneAudioSection* AudioSection : AudioSections)
	{
		if (!AudioSection)
		{
			continue;
		}

		FTrimTimes TrimTimes = CalculateTrimTimesInSection(AudioSection);
		if (TrimTimes.IsValid())
		{
			OutTrimTimesMap.Add(TrimTimes, AudioSection);
		}
	}
}

//Calculates the start and end times in milliseconds for trimming an audio section
FTrimTimes UAudioTrimmerUtilsLibrary::CalculateTrimTimesInSection(UMovieSceneAudioSection* AudioSection)
{
	const ULevelSequence* LevelSequence = AudioSection ? AudioSection->GetTypedOuter<ULevelSequence>() : nullptr;
	if (!LevelSequence)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid LevelSequence or AudioSection."));
		return FTrimTimes::Invalid;
	}

	const FFrameRate TickResolution = LevelSequence->GetMovieScene()->GetTickResolution();

	// Get the audio start offset in frames (relative to the audio asset)
	const int32 AudioStartOffsetFrames = AudioSection->GetStartOffset().Value;
	const float AudioStartOffsetSeconds = AudioStartOffsetFrames / TickResolution.AsDecimal();

	// Get the duration of the section on the track (in frames)
	const int32 SectionDurationFrames = (AudioSection->GetExclusiveEndFrame() - AudioSection->GetInclusiveStartFrame()).Value;
	const float SectionDurationSeconds = SectionDurationFrames / TickResolution.AsDecimal();

	// Calculate the effective end time within the audio asset
	float AudioEndSeconds = AudioStartOffsetSeconds + SectionDurationSeconds;

	USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
	if (!SoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("SoundWave is null or invalid."));
		return FTrimTimes::Invalid;
	}

	// Total duration of the audio in seconds
	const float TotalAudioDurationSeconds = SoundWave->Duration;

	// Check if the section is looping and handle it
	const int32 DifferenceMs = static_cast<int32>((AudioEndSeconds - TotalAudioDurationSeconds) * 1000.0f);
	const int32 MinDifferenceMs = ULevelSequencerAudioSettings::Get().MinDifferenceMs;

	if (AudioEndSeconds > TotalAudioDurationSeconds && DifferenceMs >= MinDifferenceMs)
	{
		const int32 StartFrameIndex = AudioSection->GetInclusiveStartFrame().Value;
		const int32 EndFrameIndex = AudioSection->GetExclusiveEndFrame().Value;

		UE_LOG(LogAudioTrimmer, Warning, TEXT("Audio section cannot be processed as it is looping and starts from the beginning. Level Sequence: %s, Audio Asset: %s, Section Range: %d - %d"),
		       *LevelSequence->GetName(), *SoundWave->GetName(),
		       FMath::RoundToInt(StartFrameIndex / 1000.f),
		       FMath::RoundToInt(EndFrameIndex / 1000.f));

		return FTrimTimes::Invalid;
	}

	// Clamp the end time if the difference is within the allowed threshold
	AudioEndSeconds = FMath::Min(AudioEndSeconds, TotalAudioDurationSeconds);

	// Calculate the start and end times in milliseconds
	FTrimTimes TrimTimes;
	TrimTimes.StartTimeMs = static_cast<int32>(AudioStartOffsetSeconds * 1000.0f);
	TrimTimes.EndTimeMs = static_cast<int32>(AudioEndSeconds * 1000.0f);
	TrimTimes.SoundWave = SoundWave;

	// Calculate the usage duration in milliseconds
	const int32 UsageDurationMs = TrimTimes.EndTimeMs - TrimTimes.StartTimeMs;

	// Calculate the total duration of the sound wave in milliseconds
	const int32 TotalAudioDurationMs = static_cast<int32>(TotalAudioDurationSeconds * 1000.0f);

	// Skip processing if the difference between total duration and usage duration is less than 200 milliseconds
	if (TotalAudioDurationMs - UsageDurationMs < MinDifferenceMs)
	{
		UE_LOG(LogAudioTrimmer, Log, TEXT("Skipping export for audio %s as there is almost no difference between total duration and usage duration"), *SoundWave->GetName());
		return FTrimTimes::Invalid;
	}

	// Log the start and end times in milliseconds, section duration, and percentage used
	UE_LOG(LogAudioTrimmer, Log, TEXT("Audio: %s, Used from %.2f seconds to %.2f seconds (Duration: %.2f seconds), Percentage Used: %.2f%%"),
	       *SoundWave->GetName(), AudioStartOffsetSeconds, AudioEndSeconds, AudioEndSeconds - AudioStartOffsetSeconds,
	       ((AudioEndSeconds - AudioStartOffsetSeconds) / TotalAudioDurationSeconds) * 100.0f);

	return TrimTimes;
}
