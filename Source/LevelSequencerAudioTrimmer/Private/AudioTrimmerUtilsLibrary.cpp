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

// Runs the audio trimmer for given level sequence
void UAudioTrimmerUtilsLibrary::RunLevelSequenceAudioTrimmer(const ULevelSequence* LevelSequence)
{
	// Retrieve audio sections mapped by SoundWave from the Level Sequence
	TMap<USoundWave*, TArray<UMovieSceneAudioSection*>> AudioSectionsMap;
	FindAudioSectionsInLevelSequence(AudioSectionsMap, LevelSequence);

	if (AudioSectionsMap.IsEmpty())
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("No audio sections found in the level sequence."));
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d unique sound waves."), AudioSectionsMap.Num());

	for (const TTuple<USoundWave*, TArray<UMovieSceneAudioSection*>>& It : AudioSectionsMap)
	{
		USoundWave* OriginalSoundWave = It.Key;
		const TArray<UMovieSceneAudioSection*>& Sections = It.Value;

		for (int32 Index = 0; Index < Sections.Num(); ++Index)
		{
			UMovieSceneAudioSection* AudioSection = Sections[Index];
			USoundWave* SoundWave = OriginalSoundWave;

			// Calculate trim times and check if the section should be processed
			const FTrimTimes TrimTimes = CalculateTrimTimes(LevelSequence, AudioSection);
			if (!TrimTimes.IsValid())
			{
				continue;
			}

			// Duplicate the sound wave if it's not the last section using it
			if (Index < Sections.Num() - 1)
			{
				SoundWave = DuplicateSoundWave(SoundWave, Index + 1);
				AudioSection->SetSound(SoundWave);
			}

			// Export the sound wave to a temporary WAV file
			const FString ExportPath = ExportSoundWaveToWav(SoundWave);
			if (ExportPath.IsEmpty())
			{
				UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to export %s"), *SoundWave->GetName());
				continue;
			}

			const FString TrimmedAudioPath = FPaths::ChangeExtension(ExportPath, TEXT("_trimmed.wav"));

			// Trim the audio using the C++ function
			if (!TrimAudio(TrimTimes, ExportPath, TrimmedAudioPath))
			{
				UE_LOG(LogAudioTrimmer, Warning, TEXT("Trimming audio failed for %s"), *SoundWave->GetName());
				continue;
			}

			// Only reimport the trimmed audio for the last section using the original sound wave
			if (Index == Sections.Num() - 1)
			{
				if (!ReimportAudioToUnreal(SoundWave, TrimmedAudioPath))
				{
					UE_LOG(LogAudioTrimmer, Warning, TEXT("Reimporting trimmed audio failed for %s"), *SoundWave->GetName());
					continue;
				}
			}

			// Reset the Start Frame Offset for this audio section
			ResetTrimmedAudioSection(AudioSection);

			// Delete the temporary exported WAV file
			DeleteTempWavFile(ExportPath);
			DeleteTempWavFile(TrimmedAudioPath);
		}
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Processing complete."));
}

// Retrieves all audio sections from the given level sequence
void UAudioTrimmerUtilsLibrary::FindAudioSectionsInLevelSequence(TMap<USoundWave*, TArray<UMovieSceneAudioSection*>>& OutMap, const ULevelSequence* InLevelSequence)
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
				OutMap.FindOrAdd(SoundWave).AddUnique(AudioSection);
			}
		}
	}
}

//Calculates the start and end times in milliseconds for trimming an audio section
FTrimTimes UAudioTrimmerUtilsLibrary::CalculateTrimTimes(const ULevelSequence* LevelSequence, UMovieSceneAudioSection* AudioSection)
{
	FTrimTimes TrimTimes;

	if (!LevelSequence || !AudioSection)
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

	const USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
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
	TrimTimes.StartTimeMs = static_cast<int32>(AudioStartOffsetSeconds * 1000.0f);
	TrimTimes.EndTimeMs = static_cast<int32>(AudioEndSeconds * 1000.0f);

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

// Duplicates the given SoundWave asset, incrementing an index to its name
USoundWave* UAudioTrimmerUtilsLibrary::DuplicateSoundWave(USoundWave* OriginalSoundWave, int32 DuplicateIndex)
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

// Resets the start frame offset of an audio section to zero
void UAudioTrimmerUtilsLibrary::ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection)
{
	if (!AudioSection)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid AudioSection."));
		return;
	}

	AudioSection->SetStartOffset(0);
	AudioSection->SetLooping(false);

	AudioSection->MarkAsChanged();

	// Mark the movie scene as modified
	const UMovieScene* MovieScene = AudioSection->GetTypedOuter<UMovieScene>();
	if (MovieScene)
	{
		MovieScene->MarkPackageDirty();
	}

	// Log the operation
	UE_LOG(LogAudioTrimmer, Log, TEXT("Reset Start Frame Offset and adjusted duration for section using sound: %s"), *GetNameSafe(AudioSection->GetSound()));
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
