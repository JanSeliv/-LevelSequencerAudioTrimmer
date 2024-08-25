// Copyright (c) Yevhenii Selivanov

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
//---
#include "AudioTrimmerUtilsLibrary.generated.h"

class UMovieSceneAudioSection;
class ULevelSequence;
class USoundWave;

DEFINE_LOG_CATEGORY_STATIC(LogAudioTrimmer, Log, All);

/**
 * Utility library for handling audio trimming and reimporting in Unreal Engine.
 */
UCLASS()
class LEVELSEQUENCERAUDIOTRIMMERED_API UAudioTrimmerUtilsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Runs the audio trimmer for given level sequence. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static void RunLevelSequenceAudioTrimmer(const ULevelSequence* LevelSequence);

	/** Retrieves all audio sections from the given level sequence.
	 * @param LevelSequence The level sequence to search for audio sections.
	 * @return Array of UMovieSceneAudioSection objects found within the level sequence. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static TArray<UMovieSceneAudioSection*> GetAudioSections(const ULevelSequence* LevelSequence);

	/** Calculates the start and end times in milliseconds for trimming an audio section.
	 * @param LevelSequence The level sequence containing the audio section.
	 * @param AudioSection The audio section to calculate trim times for.
	 * @param StartTimeMs Output parameter for the start time in milliseconds.
	 * @param EndTimeMs Output parameter for the end time in milliseconds. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static bool CalculateTrimTimes(const ULevelSequence* LevelSequence, UMovieSceneAudioSection* AudioSection, int32& StartTimeMs, int32& EndTimeMs);

	/** Trims an audio file to the specified start and end times.
	 * @param InputPath The file path to the audio file to trim.
	 * @param OutputPath The file path to save the trimmed audio file.
	 * @param StartTimeSec The start time in seconds to trim from.
	 * @param EndTimeSec The end time in seconds to trim to.
	 * @return True if the audio was successfully trimmed, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static bool TrimAudio(const FString& InputPath, const FString& OutputPath, float StartTimeSec, float EndTimeSec);

	/** Exports a sound wave to a WAV file.
	 * @param SoundWave The sound wave to export.
	 * @return The file path to the exported WAV file. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static FString ExportSoundWaveToWav(USoundWave* SoundWave);

	/** Reimports an audio file into the original sound wave asset in Unreal Engine.
	 * @param OriginalSoundWave The original sound wave asset to be reimported.
	 * @param TrimmedAudioFilePath The file path to the trimmed audio file.
	 * @return True if the reimport was successful, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static bool ReimportAudioToUnreal(USoundWave* OriginalSoundWave, const FString& TrimmedAudioFilePath);

	/** Resets the start frame offset of an audio section to zero.
	 * @param AudioSection The audio section to modify. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static void ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection);

	/** Deletes a temporary WAV file from the file system. * 
	 * @param FilePath The file path of the WAV file to delete.
	 * @return True if the file was successfully deleted, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static bool DeleteTempWavFile(const FString& FilePath);
};
