// Copyright (c) Yevhenii Selivanov

#pragma once

#include "CoreMinimal.h"
//---
#include "LevelSequencerAudioTypes.generated.h"

/**
 * Represents the start and end times in milliseconds for trimming an audio section.
 */
USTRUCT(BlueprintType)
struct LEVELSEQUENCERAUDIOTRIMMERED_API FTrimTimes
{
	GENERATED_BODY()

	FTrimTimes() = default;
	FTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs);

	/** Invalid trim times. */
	static const FTrimTimes Invalid;

	/** Start time in milliseconds to trim from. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 StartTimeMs = 0;

	/** End time in milliseconds to trim to. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Audio Trimmer")
	int32 EndTimeMs = 0;

	/** Returns true if the start and end times are valid. */
	bool IsValid() const;

	/** Returns true if the start and end times are similar to the other trim times within the given tolerance. */
	bool IsSimilar(const FTrimTimes& Other, int32 ToleranceMs = KINDA_SMALL_NUMBER) const;

	/** Equal operator for comparing in TMap. */
	bool operator==(const FTrimTimes& Other) const;

	/** Hash function to TMap. */
	friend LEVELSEQUENCERAUDIOTRIMMERED_API uint32 GetTypeHash(const FTrimTimes& TrimTimes);
};
