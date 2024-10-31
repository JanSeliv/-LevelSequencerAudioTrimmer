// Copyright (c) Yevhenii Selivanov

#pragma once

#include "LSATTrimTimesData.generated.h"

class USoundWave;

struct FFrameRate;

/**
 * Represents the start and end times in milliseconds for trimming an audio section.
 */
USTRUCT(BlueprintType, meta = (HasNativeMake = "/Script/LevelSequencerAudioTrimmerEd.LSATUtilsLibrary.MakeTrimTimes"))
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimes
{
	GENERATED_BODY()

	FLSATTrimTimes() = default;
	FLSATTrimTimes(const class UMovieSceneAudioSection* AudioSection);
	FLSATTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs, USoundWave* InSoundWave);

	/*********************************************************************************************
	 * Trim Times Data
	 ********************************************************************************************* */
public:
	/** Invalid trim times. */
	static const FLSATTrimTimes Invalid;

	FORCEINLINE int32 GetSoundTrimStartMs() const { return SoundTrimStartMs; }
	FORCEINLINE int32 GetSoundTrimEndMs() const { return SoundTrimEndMs; }

	FORCEINLINE USoundWave* GetSoundWave() const { return SoundWave.Get(); }
	FORCEINLINE void SetSoundWave(USoundWave* InSoundWave) { SoundWave = InSoundWave; }

protected:
	/** The start time in milliseconds from the sound asset where trimming begins.
	* This value represents the point in the sound where playback starts after the audio section has been trimmed from the left,
	meaning the sound does not necessarily start from its original beginning. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 SoundTrimStartMs = 0;

	/** The end time in milliseconds of the sound asset where trimming ends.
	* This represents the last used portion of the sound before the audio section finishes or is trimmed on the right,
	meaning the sound may end before reaching its full duration. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 SoundTrimEndMs = 0;

	/** The sound wave associated with these trim times. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TObjectPtr<USoundWave> SoundWave = nullptr;

	/*********************************************************************************************
	 * Trim Times Helpers
	 ********************************************************************************************* */
public:
	/** Returns SoundTrimStartMs in seconds. */
	FORCEINLINE float GetSoundTrimStartSeconds() const { return static_cast<float>(SoundTrimStartMs) / 1000.f; }

	/** Returns SoundTrimEndMs in seconds. */
	FORCEINLINE float GetSoundTrimEndSeconds() const { return static_cast<float>(SoundTrimEndMs) / 1000.f; }

	/** Returns SoundTrimStartMs in frames based on the tick resolution, or -1 if cannot convert. */
	int32 GetSoundTrimStartFrame(const FFrameRate& TickResolution) const;

	/** Returns SoundTrimEndMs in frames based on the tick resolution, or -1 if cannot convert. */
	int32 GetSoundTrimEndFrame(const FFrameRate& TickResolution) const;

	/** Returns true if the audio section is looping (repeating playing from the start). */
	bool IsLooping() const;

	/** Returns the duration of actual usage in milliseconds. */
	FORCEINLINE int32 GetUsageDurationMs() const { return SoundTrimEndMs - SoundTrimStartMs; }

	/** Returns the usage percentage of the sound wave asset in 0-100 range. */
	float GetUsagePercentage() const;

	/** Returns the number of frames the sound wave asset is used. */
	int32 GetUsagesFrames(const FFrameRate& TickResolution) const;

	/** Returns the total duration of the sound wave asset in milliseconds, it might be different from the actual usage duration. */
	int32 GetSoundTotalDurationMs() const;

	/** Returns true if the sound is already trimmer, so usage duration and total duration are similar. */
	bool IsSoundTrimmed() const;

	/** Returns true if all data is set and valid. */
	bool IsValid() const;

	/** Returns true if duration is valid and positive. */
	bool IsValidLength(const FFrameRate& TickResolution) const;

	/** Checks if the trim times are within the bounds of the given audio section. */
	bool IsWithinSectionBounds(const class UMovieSceneAudioSection* AudioSection) const;

	/** Checks if the trim times are within the original trim times.*/
	bool IsWithinTrimBounds(const FLSATTrimTimes& OtherTrimTimes) const;

	/** Returns larger mix of the two trim times: larger start time from both and larger end time from both. */
	static FLSATTrimTimes GetMaxTrimTimes(const FLSATTrimTimes& Left, const FLSATTrimTimes& Right);

	/** Returns the string representation of the trim times that might be useful for logging. */
	FString ToString(const FFrameRate& TickResolution) const;

	/** Returns short string representation of the trim times. */
	FString ToCompactString() const;

	/*********************************************************************************************
	 * Trim Times Operators
	 ********************************************************************************************* */
public:
	/** Equal operator for comparing in TMap. */
	bool operator==(const FLSATTrimTimes& Other) const;

	/** Hash function to TMap. */
	friend LEVELSEQUENCERAUDIOTRIMMERED_API uint32 GetTypeHash(const FLSATTrimTimes& TrimTimes);
};

/**
 * Contains the array of audio sections to trim.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATSectionsContainer
{
	GENERATED_BODY()

	/** Sets the sound wave for all audio sections in this container. */
	void SetSound(USoundWave* SoundWave);

	auto begin() const { return AudioSections.begin(); }
	auto end() const { return AudioSections.end(); }

	bool Add(UMovieSceneAudioSection* AudioSection);
	FORCEINLINE int32 Num() const { return AudioSections.Num(); }
	FORCEINLINE bool IsEmpty() const { return AudioSections.IsEmpty(); }
	FORCEINLINE bool Contains(const UMovieSceneAudioSection* AudioSection) const { return AudioSections.Contains(AudioSection); }

	void Append(const FLSATSectionsContainer& Other);

protected:
	/** The array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TArray<TObjectPtr<class UMovieSceneAudioSection>> AudioSections;
};

/** Custom function to process each audio section in this map.
 * The Processor should use OutAllNewSections to store new sections created during processing if any.
 * @param AudioSection The audio section to be processed.
 * @param TrimTimes The TrimTimes associated with the audio section.
 * @param OutAllNewSections Used to collect new sections created by the Processor. */
typedef TFunctionRef<void(UMovieSceneAudioSection* /*AudioSection*/, const FLSATTrimTimes& /*TrimTimes*/, FLSATSectionsContainer& /*OutAllNewSections*/)> FLSATSectionsProcessor;

/**
 * Represents the map of trim times to the container of audio sections to trim.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimesMap
{
	GENERATED_BODY()

	/** Returns first audio section from the audio sections container. */
	const UMovieSceneAudioSection* GetFirstAudioSection() const;

	/** Sets the sound wave for all trim times and audio sections in this map. */
	void SetSound(USoundWave* SoundWave);

	/** Is supposed to be called when needed to change any values inside the map, instead of changing them directly.
	 * Processes each audio section in this map and rebuilds the map.
	 * The Processor is expected to modify, create or split new sections adding them to OutAllNewSections.
	 * Map will be recalculated if only OutAllNewSections is not empty.
	 * @param Processor The custom function to process each audio section. */
	void RebuildTrimTimesMapWithProcessor(const FLSATSectionsProcessor& Processor);

	auto begin() const { return TrimTimesMap.begin(); }
	auto end() const { return TrimTimesMap.end(); }

	FORCEINLINE int32 Num() const { return TrimTimesMap.Num(); }
	FORCEINLINE bool IsEmpty() const { return TrimTimesMap.IsEmpty(); }
	void GetKeys(TArray<FLSATTrimTimes>& OutKeys) const { TrimTimesMap.GetKeys(OutKeys); }

	bool Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection);
	FLSATSectionsContainer& Add(const FLSATTrimTimes& TrimTimes);
	FLSATSectionsContainer& Add(const FLSATTrimTimes& TrimTimes, const FLSATSectionsContainer& SectionsContainer);

	void Remove(const FLSATTrimTimes& TrimTimes) { TrimTimesMap.Remove(TrimTimes); }

	void SortKeys();

protected:
	/** The key is the trim times, and the value is the array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<FLSATTrimTimes, FLSATSectionsContainer> TrimTimesMap;
};

/**
 * Represents the map of sound waves to their corresponding trim times map.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimesMultiMap
{
	GENERATED_BODY()

	/** Returns all sounds waves from this multimap that satisfies the given predicate. */
	void GetSounds(TArray<USoundWave*>& OutSoundWaves, const TFunctionRef<bool(const TTuple<FLSATTrimTimes, FLSATSectionsContainer>&)>& Predicate) const;

	auto begin() const { return TrimTimesMultiMap.begin(); }
	auto end() const { return TrimTimesMultiMap.end(); }
	auto begin() { return TrimTimesMultiMap.begin(); }
	auto end() { return TrimTimesMultiMap.end(); }

	FORCEINLINE int32 Num() const { return TrimTimesMultiMap.Num(); }
	FORCEINLINE bool IsEmpty() const { return TrimTimesMultiMap.IsEmpty(); }
	FORCEINLINE FLSATTrimTimesMap& FindOrAdd(USoundWave* SoundWave) { return TrimTimesMultiMap.FindOrAdd(SoundWave); }
	FORCEINLINE FLSATTrimTimesMap& Add(USoundWave* SoundWave, const FLSATTrimTimesMap& TrimTimesMap) { return TrimTimesMultiMap.Add(SoundWave, TrimTimesMap); }
	void Remove(const USoundWave* SoundWave) { TrimTimesMultiMap.Remove(SoundWave); }
	void Remove(const TArray<USoundWave*>& SoundWaves);

protected:
	/** The key is the sound wave, and the value is the map of trim times to the container of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<TObjectPtr<USoundWave>, FLSATTrimTimesMap> TrimTimesMultiMap;
};
