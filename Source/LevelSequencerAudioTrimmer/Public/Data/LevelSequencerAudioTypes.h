// Copyright (c) Yevhenii Selivanov

#pragma once

#include "CoreMinimal.h"
//---
#include "LevelSequencerAudioTypes.generated.h"

class USoundWave;

/**
 * Represents the start and end times in milliseconds for trimming an audio section.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FTrimTimes
{
	GENERATED_BODY()

	FTrimTimes() = default;
	FTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs, USoundWave* InSoundWave);

	/** Invalid trim times. */
	static const FTrimTimes Invalid;

	/** Start time in milliseconds to trim from. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 StartTimeMs = 0;

	/** End time in milliseconds to trim to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 EndTimeMs = 0;

	/** The sound wave associated with these trim times. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TObjectPtr<USoundWave> SoundWave = nullptr;

	/** Returns true if the start and end times are valid. */
	bool IsValid() const;

	/** Returns true if the start and end times are similar to the other trim times within the given tolerance. */
	bool IsSimilar(const FTrimTimes& Other, int32 ToleranceMs) const;

	/** Equal operator for comparing in TMap. */
	bool operator==(const FTrimTimes& Other) const;

	/** Hash function to TMap. */
	friend LEVELSEQUENCERAUDIOTRIMMERED_API uint32 GetTypeHash(const FTrimTimes& TrimTimes);
};

/**
 * Contains the array of audio sections to trim.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FAudioSectionsContainer
{
	GENERATED_BODY()

	/** The array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TArray<TObjectPtr<class UMovieSceneAudioSection>> AudioSections;

	auto begin() const { return AudioSections.begin(); }
	auto end() const { return AudioSections.end(); }
	auto begin() { return AudioSections.begin(); }
	auto end() { return AudioSections.end(); }

	bool Add(UMovieSceneAudioSection* AudioSection);
};

/**
 * Represents the map of trim times to the container of audio sections to trim.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FTrimTimesMap
{
	GENERATED_BODY()

	/** The key is the trim times, and the value is the array of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<FTrimTimes, FAudioSectionsContainer> TrimTimesMap;

	auto begin() const { return TrimTimesMap.begin(); }
	auto end() const { return TrimTimesMap.end(); }
	auto begin() { return TrimTimesMap.begin(); }
	auto end() { return TrimTimesMap.end(); }

	FORCEINLINE int32 Num() const { return TrimTimesMap.Num(); }
	bool Add(const FTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection);

	/** Returns the first level sequence from the audio sections container. */
	class ULevelSequence* GetFirstLevelSequence() const;
};

/**
 * Represents the map of sound waves to their corresponding trim times map.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FSoundsTrimTimesMap
{
	GENERATED_BODY()

	/** The key is the sound wave, and the value is the map of trim times to the container of audio sections to trim. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	TMap<TObjectPtr<USoundWave>, FTrimTimesMap> SoundsTrimTimesMap;

	auto begin() const { return SoundsTrimTimesMap.begin(); }
	auto end() const { return SoundsTrimTimesMap.end(); }
	auto begin() { return SoundsTrimTimesMap.begin(); }
	auto end() { return SoundsTrimTimesMap.end(); }

	FORCEINLINE int32 Num() const { return SoundsTrimTimesMap.Num(); }
	FORCEINLINE bool IsEmpty() const { return SoundsTrimTimesMap.IsEmpty(); }
	FORCEINLINE FTrimTimesMap& FindOrAdd(USoundWave* SoundWave) { return SoundsTrimTimesMap.FindOrAdd(SoundWave); }
};
