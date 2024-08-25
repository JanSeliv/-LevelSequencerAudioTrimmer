// Copyright (c) Yevhenii Selivanov

#include "AudioTrimmerUtilsLibrary.h"
//---
#include "AssetExportTask.h"
#include "AssetToolsModule.h"
#include "LevelSequence.h"
#include "LevelSequencerAudioTrimmerEdModule.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "ObjectTools.h"
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
#include "Tracks/MovieSceneSubTrack.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(AudioTrimmerUtilsLibrary)

// Runs the audio trimmer for given level sequence
void UAudioTrimmerUtilsLibrary::RunLevelSequenceAudioTrimmer(const ULevelSequence* LevelSequence)
{
	// Retrieve all audio sections from the Level Sequence
	TArray<UMovieSceneAudioSection*> AudioSections = GetAudioSections(LevelSequence);

	if (AudioSections.Num() == 0)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("No audio sections found in the level sequence."));
		return;
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Found %d audio sections."), AudioSections.Num());

	for (UMovieSceneAudioSection* AudioSection : AudioSections)
	{
		USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
		if (!SoundWave)
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to get SoundWave from AudioSection. Skipping..."));
			continue;
		}

		// Calculate trim times and check if the section should be processed
		int32 StartTimeMs, EndTimeMs;
		if (!CalculateTrimTimes(LevelSequence, AudioSection, StartTimeMs, EndTimeMs))
		{
			continue;
		}

		// Export the sound wave to a temporary WAV file
		FString ExportPath = ExportSoundWaveToWav(SoundWave);
		if (ExportPath.IsEmpty())
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Failed to export %s. Skipping..."), *SoundWave->GetName());
			continue;
		}

		const float StartTimeSec = StartTimeMs / 1000.0f;
		const float EndTimeSec = EndTimeMs / 1000.0f;
		FString TrimmedAudioPath = FPaths::ChangeExtension(ExportPath, TEXT("_trimmed.wav"));

		// Trim the audio using the C++ function
		if (!TrimAudio(ExportPath, TrimmedAudioPath, StartTimeSec, EndTimeSec))
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Trimming audio failed for %s. Skipping..."), *SoundWave->GetName());
			continue;
		}

		// Reimport the trimmed audio into the original sound wave asset using FReimportManager
		if (!ReimportAudioToUnreal(SoundWave, TrimmedAudioPath))
		{
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Reimporting trimmed audio failed for %s. Skipping..."), *SoundWave->GetName());
			continue;
		}

		// Reset the Start Frame Offset for this audio section
		ResetTrimmedAudioSection(AudioSection);

		// Delete the temporary exported WAV file
		DeleteTempWavFile(ExportPath);
		DeleteTempWavFile(TrimmedAudioPath);
	}

	UE_LOG(LogAudioTrimmer, Log, TEXT("Processing complete."));
}

// Retrieves all audio sections from the given level sequence
TArray<UMovieSceneAudioSection*> UAudioTrimmerUtilsLibrary::GetAudioSections(const ULevelSequence* LevelSequence)
{
	if (!LevelSequence)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid LevelSequence."));
		return {};
	}

	TArray<UMovieSceneAudioSection*> AudioSections;
	for (UMovieSceneTrack* Track : LevelSequence->GetMovieScene()->GetTracks())
	{
		if (const UMovieSceneAudioTrack* AudioTrack = Cast<UMovieSceneAudioTrack>(Track))
		{
			for (UMovieSceneSection* Section : AudioTrack->GetAllSections())
			{
				if (UMovieSceneAudioSection* AudioSection = Cast<UMovieSceneAudioSection>(Section))
				{
					AudioSections.Add(AudioSection);
				}
			}
		}
	}

	return AudioSections;
}

//Calculates the start and end times in milliseconds for trimming an audio section
bool UAudioTrimmerUtilsLibrary::CalculateTrimTimes(const ULevelSequence* LevelSequence, UMovieSceneAudioSection* AudioSection, int32& StartTimeMs, int32& EndTimeMs)
{
	if (!LevelSequence || !AudioSection)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid LevelSequence or AudioSection."));
		return false;
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

	const USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
	{
		// Total duration of the audio in seconds
		const float TotalAudioDurationSeconds = SoundWave->Duration;

		// Check if the section is looping
		if (AudioEndSeconds > TotalAudioDurationSeconds)
		{
			// Directly use the frame numbers from the section
			const int32 StartFrameIndex = AudioSection->GetInclusiveStartFrame().Value;
			const int32 EndFrameIndex = AudioSection->GetExclusiveEndFrame().Value;

			// Log the rounded frame numbers divided by 1000
			UE_LOG(LogAudioTrimmer, Warning, TEXT("Audio section cannot be processed as it is looping and starts from the beginning. Level Sequence: %s, Audio Asset: %s, Section Range: %d - %d"),
			       *LevelSequence->GetName(), *SoundWave->GetName(),
			       FMath::RoundToInt(StartFrameIndex / 1000.f),
			       FMath::RoundToInt(EndFrameIndex / 1000.f));

			return false;
		}

		// Calculate the start and end times in milliseconds
		StartTimeMs = static_cast<int32>(AudioStartOffsetSeconds * 1000.0f);
		EndTimeMs = static_cast<int32>(AudioEndSeconds * 1000.0f);

		// Calculate the usage duration in milliseconds
		const int32 UsageDurationMs = EndTimeMs - StartTimeMs;

		// Calculate the total duration of the sound wave in milliseconds
		const int32 TotalAudioDurationMs = static_cast<int32>(TotalAudioDurationSeconds * 1000.0f);

		// Check if the difference between total duration and usage duration is less than 200 milliseconds
		const int32 DifferenceMs = TotalAudioDurationMs - UsageDurationMs;

		constexpr int32 MinDifferenceMs = 200;
		if (DifferenceMs < MinDifferenceMs)
		{
			UE_LOG(LogAudioTrimmer, Log, TEXT("Skipping export for audio %s as there is almost no difference between total duration and usage duration"), *SoundWave->GetName());
			return false;
		}

		// Log the start and end times in milliseconds, section duration, and percentage used
		UE_LOG(LogAudioTrimmer, Log, TEXT("Audio: %s, Used from %.2f seconds to %.2f seconds (Duration: %.2f seconds), Percentage Used: %.2f%%"),
		       *SoundWave->GetName(), AudioStartOffsetSeconds, AudioEndSeconds, AudioEndSeconds - AudioStartOffsetSeconds,
		       ((AudioEndSeconds - AudioStartOffsetSeconds) / TotalAudioDurationSeconds) * 100.0f);

		return true;
	}

	UE_LOG(LogAudioTrimmer, Warning, TEXT("SoundWave is null or invalid."));
	return false;
}

// Trims an audio file to the specified start and end times
bool UAudioTrimmerUtilsLibrary::TrimAudio(const FString& InputPath, const FString& OutputPath, float StartTimeSec, float EndTimeSec)
{
	int32 ReturnCode;
	FString Output;
	FString Errors;

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

	const USoundWave* SoundWave = Cast<USoundWave>(AudioSection->GetSound());
	if (!SoundWave)
	{
		UE_LOG(LogAudioTrimmer, Warning, TEXT("Invalid SoundWave in AudioSection."));
		return;
	}

	AudioSection->SetStartOffset(0);
	AudioSection->SetLooping(false);

	// Adjust the section duration to match the trimmed audio duration
	const FFrameRate TickResolution = AudioSection->GetTypedOuter<UMovieScene>()->GetTickResolution();
	const FFrameTime NewEndFrameTime = FFrameTime::FromDecimal(SoundWave->Duration * TickResolution.AsDecimal());
	const FFrameNumber NewEndFrame = AudioSection->GetInclusiveStartFrame() + NewEndFrameTime.GetFrame();
	AudioSection->SetEndFrame(NewEndFrame);

	AudioSection->MarkAsChanged();

	// Mark the movie scene as modified
	const UMovieScene* MovieScene = AudioSection->GetTypedOuter<UMovieScene>();
	if (MovieScene)
	{
		MovieScene->MarkPackageDirty();
	}

	// Log the operation
	UE_LOG(LogAudioTrimmer, Log, TEXT("Reset Start Frame Offset and adjusted duration for section using sound: %s"), *SoundWave->GetName());
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
