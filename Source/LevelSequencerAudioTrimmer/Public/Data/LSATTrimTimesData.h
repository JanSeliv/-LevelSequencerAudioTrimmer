// Copyright (c) Yevhenii Selivanov

#pragma once

#include "CoreMinimal.h"
//---
#include "LSATTrimTimesData.generated.h"

class USoundWave;

/**
 * Represents the start and end times in milliseconds for trimming an audio section.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FLSATTrimTimes
{
	GENERATED_BODY()

	FLSATTrimTimes() = default;
	FLSATTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs);

	/** Invalid trim times. */
	static const FLSATTrimTimes Invalid;

	/** Start time in milliseconds to trim from. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 StartTimeMs = 0;

	/** End time in milliseconds to trim to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 EndTimeMs = 0;

	/** The sound wave associated with these trim times. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TObjectPtr<USoundWave> SoundWave = nullptr;

	/** The track on level sequence associated with these trim times. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TObjectPtr<class UMovieSceneAudioSection> AudioSection = nullptr;

	/** The level sequence associated with these trim times. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TObjectPtr<class ULevelSequence> LevelSequence = nullptr;

	/** Returns true if the audio section is looping (repeating playing from the start). */
	bool IsLooping() const;

	/** Returns the duration of actual usage in milliseconds. */
	FORCEINLINE int32 GetUsageDurationMs() const { return EndTimeMs - StartTimeMs; }

	/** Returns the total duration of the sound wave asset in milliseconds, it might be different from the actual usage duration. */
	int32 GetTotalDurationMs() const;

	/** Returns true if usage duration and total duration are similar. */
	bool IsUsageSimilarToTotalDuration() const;

	/** Returns true if the start and end times are valid. */
	bool IsValid() const;

	/** Returns true if the start and end times are similar to the other trim times within the given tolerance. */
	bool IsSimilar(const FLSATTrimTimes& Other, int32 ToleranceMs) const;

	/** Returns the string representation of the trim times that might be useful for logging. */
	FString ToString() const;

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

	/** Returns the first level sequence from the audio sections container. */
	class ULevelSequence* GetFirstLevelSequence() const;

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
	void GetSounds(TArray<USoundWave*>& OutSoundWaves, TFunctionRef<bool(const TTuple<FLSATTrimTimes, FLSATSectionsContainer>&)> Predicate) const;

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
