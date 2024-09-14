// Copyright (c) Yevhenii Selivanov

#pragma once

#include "CoreMinimal.h"
//---
#include "LSATTrimTimesData.generated.h"

class USoundWave;

struct FFrameRate;

/**
 * Represents the start and end times in milliseconds for trimming an audio section.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimes
{
	GENERATED_BODY()

	FLSATTrimTimes() = default;
	FLSATTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs);

	/*********************************************************************************************
	 * Trim Times Data
	 ********************************************************************************************* */

	/** Invalid trim times. */
	static const FLSATTrimTimes Invalid;

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
	 * Trim Times Methods
	 ********************************************************************************************* */

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

	/** Returns true if the start and end times are valid. */
	bool IsValid() const;

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

	/** The array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TArray<TObjectPtr<class UMovieSceneAudioSection>> AudioSections;

	/** Sets the sound wave for all audio sections in this container. */
	void SetSound(USoundWave* SoundWave);

	auto begin() const { return AudioSections.begin(); }
	auto end() const { return AudioSections.end(); }
	auto begin() { return AudioSections.begin(); }
	auto end() { return AudioSections.end(); }

	bool Add(UMovieSceneAudioSection* AudioSection);
	int32 Num() const { return AudioSections.Num(); }
	bool IsEmpty() const { return AudioSections.IsEmpty(); }
	void Append(const FLSATSectionsContainer& Other) { AudioSections.Append(Other.AudioSections); }
};

/**
 * Represents the map of trim times to the container of audio sections to trim.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimesMap
{
	GENERATED_BODY()

	/** The key is the trim times, and the value is the array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<FLSATTrimTimes, FLSATSectionsContainer> TrimTimesMap;

	/** Returns first audio section from the audio sections container. */
	UMovieSceneAudioSection* GetFirstAudioSection() const;

	/** Sets the sound wave for all trim times and audio sections in this map. */
	void SetSound(USoundWave* SoundWave);

	auto begin() const { return TrimTimesMap.begin(); }
	auto end() const { return TrimTimesMap.end(); }
	auto begin() { return TrimTimesMap.begin(); }
	auto end() { return TrimTimesMap.end(); }

	FORCEINLINE int32 Num() const { return TrimTimesMap.Num(); }
	FORCEINLINE bool IsEmpty() const { return TrimTimesMap.IsEmpty(); }
	bool Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection);
	FLSATSectionsContainer& Add(const FLSATTrimTimes& TrimTimes, const FLSATSectionsContainer& SectionsContainer);
	void Remove(const FLSATTrimTimes& TrimTimes) { TrimTimesMap.Remove(TrimTimes); }
};

/**
 * Represents the map of sound waves to their corresponding trim times map.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimesMultiMap
{
	GENERATED_BODY()

	/** The key is the sound wave, and the value is the map of trim times to the container of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<TObjectPtr<USoundWave>, FLSATTrimTimesMap> TrimTimesMultiMap;

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
};
