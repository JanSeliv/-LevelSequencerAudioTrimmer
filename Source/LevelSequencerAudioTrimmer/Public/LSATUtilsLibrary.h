// Copyright (c) Yevhenii Selivanov

#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
//---
#include "LSATUtilsLibrary.generated.h"

class UMovieSceneSection;
class UMovieSceneAudioSection;
class ULevelSequence;
class USoundWave;

struct FLSATTrimTimes;
struct FLSATSectionsContainer;
struct FLSATTrimTimesMap;
struct FLSATTrimTimesMultiMap;
struct FFrameRate;

DEFINE_LOG_CATEGORY_STATIC(LogAudioTrimmer, Log, All);

/**
 * Utility library for handling audio trimming and reimporting in Unreal Engine.
 */
UCLASS()
class LEVELSEQUENCERAUDIOTRIMMERED_API ULSATUtilsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Entry method to run the main flow of trimming all audio assets for the given level sequence.
	 * - Trims audio assets based on their usage in given Level Sequence to reduce the file size.
	 * - Reuses already trimmed audio assets if they are used multiple times with the same trim times.
	 * - Duplicates sound waves if needed to handle multiple instances of the same audio with different trim times. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static void RunLevelSequenceAudioTrimmer(const TArray<ULevelSequence*>& LevelSequences);

	/*********************************************************************************************
	 * Gathering Sounds
	 * Prepares the `TrimTimesMultiMap` map that combines sound waves with their corresponding trim times.
	 ********************************************************************************************* */
public:
	/** Prepares a map of sound waves to their corresponding trim times based on the audio sections used in the given level sequence.
	 * @param InOutTrimTimesMultiMap Combines and returns a map of sound waves to their corresponding trim times.
	 * @param LevelSequence The main level sequence to search for audio sections. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Preprocessing")
	static void GatherSoundsInRequestedLevelSequence(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap, const ULevelSequence* LevelSequence);

	/** Handles those sounds from requested Level Sequence that are used at the same time in other Level Sequences.
	* @param InOutTrimTimesMultiMap Takes the map of sound waves and adds the trim times with sections of the sound waves that are used in other sequences. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Preprocessing")
	static void GatherSoundsInOtherSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/** Main goal of this function is to handle those sounds that are used outside of level sequences like in the world or blueprints.
	 * @param InOutTrimTimesMultiMap Takes the map of sound waves and modifies it if matches found with external used sounds.  */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Preprocessing")
	static void GatherSoundsOutsideSequences(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/*********************************************************************************************
	 * Preprocessing
	 * Initial modifying of the `TrimTimesMultiMap` map based on the gathered sounds. 
	 ********************************************************************************************* */

	/** Trims the audio tracks by level sequence boundaries, so the audio is not played outside of the level sequence.
	 * @param InOutTrimTimesMultiMap Takes the map of sound waves and modifies it based on the trim times. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Preprocessing")
	static void HandleTrackBoundaries(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/** Handles cases where the start offset is larger than the total length of the audio.
     * @param InOutTrimTimesMultiMap Takes the map of sound waves and modifies it based on the start offset. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Preprocessing")
	static void HandleLargeStartOffset(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/** Handles the policy for looping sounds based on the settings, e.g: skipping all looping sounds.
	 * @param InOutTrimTimesMultiMap Takes the map of sound waves and modifies it based on the policy for looping sounds. */
	static void HandlePolicyLoopingSounds(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/** Handles the reuse and fragmentation of sound segments within a level sequence.
	 * @param InOutTrimTimesMultiMap Takes the map of sound waves and modifies it according to the segment reuse policy. */
	static void HandlePolicyFragmentation(FLSATTrimTimesMultiMap& InOutTrimTimesMultiMap);

	/*********************************************************************************************
	 * Main Flow
	 * Is called after the preprocessing for each found audio.
	 ********************************************************************************************* */
public:
	/** Exports a sound wave to a WAV file.
	 * @param SoundWave The sound wave to export.
	 * @return The file path to the exported WAV file. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow")
	static FString ExportSoundWaveToWav(USoundWave* SoundWave);

	/** Trims an audio file to the specified start and end times.
	 * @param TrimTimes The start and end times in milliseconds to trim the audio file to.
	 * @param InputPath The file path to the audio file to trim.
	 * @param OutputPath The file path to save the trimmed audio file.
	 * @return True if the audio was successfully trimmed, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow", meta = (AutoCreateRefTerm = "TrimTimes,InputPath,OutputPath"))
	static bool TrimAudio(const FLSATTrimTimes& TrimTimes, const FString& InputPath, const FString& OutputPath);

	/** Reimports an audio file into the original sound wave asset in Unreal Engine.
	 * @param OriginalSoundWave The original sound wave asset to be reimported.
	 * @param TrimmedAudioFilePath The file path to the trimmed audio file.
	 * @return True if the reimport was successful, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow", meta = (AutoCreateRefTerm = "TrimmedAudioFilePath"))
	static bool ReimportAudioToUnreal(USoundWave* OriginalSoundWave, const FString& TrimmedAudioFilePath);

	/** Resets the start frame offset of an audio section to 0 and sets a new sound wave.
	 * @param AudioSection The audio section to modify.
	 * @param OptionalNewSound The new sound wave to set for the audio section. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer")
	static void ResetTrimmedAudioSection(UMovieSceneAudioSection* AudioSection, USoundWave* OptionalNewSound = nullptr);

	/** Deletes a temporary WAV file from the file system. * 
	 * @param FilePath The file path of the WAV file to delete.
	 * @return True if the file was successfully deleted, false otherwise. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow", meta = (AutoCreateRefTerm = "FilePath"))
	static bool DeleteTempWavFile(const FString& FilePath);

	/*********************************************************************************************
	 * Helpers
	 ********************************************************************************************* */
public:
	/** Duplicates the given SoundWave asset, incrementing an index to its name.
	 * Useful for handling multiple instances of the same audio with different trim times.
	 * @param OriginalSoundWave The original SoundWave asset to duplicate.
	 * @param DuplicateIndex The index to append to the duplicated asset's name.
	 * @return Duplicated Sound Wave asset in the same directory as the original asset. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow")
	static USoundWave* DuplicateSoundWave(USoundWave* OriginalSoundWave, int32 DuplicateIndex = 1);

	/** Duplicates the given audio section in the specified start and end frames.
	 * @param OriginalAudioSection The original audio section to duplicate.
	 * @param SectionStart The start frame to trim the audio section to.
	 * @param SectionEnd The end frame to trim the audio section to.
	 * @param SoundStartOffset The start frame offset of the sound wave. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Main Flow")
	static UMovieSceneAudioSection* DuplicateAudioSection(UMovieSceneAudioSection* OriginalAudioSection, FFrameNumber SectionStart, FFrameNumber SectionEnd, FFrameNumber SoundStartOffset);

	/** Retrieves all audio sections from the given level sequence.
	 * @param OutMap Returns a map of sound waves to their corresponding audio sections, where the same sound wave can be used in multiple audio sections.
	 * @param InLevelSequence The level sequence to search for audio sections. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static void FindAudioSectionsInLevelSequence(TMap<USoundWave*, FLSATSectionsContainer>& OutMap, const ULevelSequence* InLevelSequence);

	/** Finds all assets that directly reference the given sound wave.
	* @param OutUsages Returns the found assets that reference the sound wave.
	* @param InSound The sound wave asset to find usages for. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static void FindAudioUsagesBySoundAsset(TArray<UObject*>& OutUsages, const USoundWave* InSound);

	/** Calculates the start and end times in milliseconds for trimming multiple audio sections.
	 * @param OutTrimTimesMap Returns a map of audio sections to their corresponding trim times.
	 * @param AudioSections The audio sections to calculate trim times for. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Utilities", meta = (AutoCreateRefTerm = "AudioSections"))
	static void CalculateTrimTimesInAllSections(FLSATTrimTimesMap& OutTrimTimesMap, const FLSATSectionsContainer& AudioSections);

	/** Calculates the start and end times in milliseconds for trimming an audio section.
	 * @param AudioSection The audio section to calculate trim times for.
	 * @return A struct containing the start and end times in milliseconds. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static FLSATTrimTimes MakeTrimTimes(const UMovieSceneAudioSection* AudioSection);

	/** Splits the looping segments in the given trim times into multiple sections.
	 * Appends the original and newly created sections to the output container.
	 * @param OutNewSectionsContainer Returns original and new sections.
	 * @param InAudioSection The audio section to split.
	 * @param TrimTimes Specifies the trim times for the section to be split. */
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Utilities")
	static void SplitLoopingSection(FLSATSectionsContainer& OutNewSectionsContainer, UMovieSceneAudioSection* InAudioSection, const FLSATTrimTimes& TrimTimes);

	/** Returns the actual start time of the audio section in the level Sequence in milliseconds, otherwise -1. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static int32 GetSectionInclusiveStartTimeMs(const UMovieSceneSection* InSection);

	/** Returns the actual end time of the audio section in the level Sequence in milliseconds, otherwise -1. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static int32 GetSectionExclusiveEndTimeMs(const UMovieSceneSection* InSection);

	/** Converts seconds to frames based on the frame rate, or -1 if cannot convert. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static int32 ConvertMsToFrame(int32 InMilliseconds, const FFrameRate& TickResolution);
	static FFrameNumber ConvertMsToFrameNumber(int32 InMilliseconds, const FFrameRate& TickResolution);

	/** Converts frames to milliseconds based on the frame rate, or -1 if cannot convert. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static int32 ConvertFrameToMs(const FFrameNumber& InFrame, const FFrameRate& TickResolution);

	/** Returns the tick resolution of the given section. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static FFrameRate GetTickResolution(const UMovieSceneSection* InSection);

	/** Returns the Level Sequence of the given section. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static class ULevelSequence* GetLevelSequence(const UMovieSceneSection* InSection);

	/** Splits the given trim times into smaller, non-overlapping parts that can be reused.
	 * @param InOutTrimTimes Takes the trim times and modifies them to be non-overlapping and reusable.
	 * @param SoundWave The sound wave asset to split the trim times for. */
	UFUNCTION(BlueprintPure, Category = "Audio Trimmer|Utilities")
	static void GetFragmentedTrimTimes(TArray<FLSATTrimTimes>& InOutTrimTimes, USoundWave* SoundWave);

	/** Creates new audio sections by duplicating the original section based on the provided trim times, adjusting start and end times to fit within the valid range.
	 * @param OriginalAudioSection The original audio section that will be duplicated and fragmented.
	 * @param InTrimTimes The array of new trim times used to fragment the original section into smaller pieces.
	 * @param OutAllNewSections The container that will store all newly created audio sections based on the trim times.
	 * @param InRange The original trim times of the audio section to ensure all fragments stay within these bounds.*/
	UFUNCTION(BlueprintCallable, Category = "Audio Trimmer|Utilities")
	static void CreateAudioSectionsByTrimTimes(UMovieSceneAudioSection* OriginalAudioSection, const TArray<FLSATTrimTimes>& InTrimTimes, FLSATSectionsContainer& OutAllNewSections, const FLSATTrimTimes& InRange);
};
