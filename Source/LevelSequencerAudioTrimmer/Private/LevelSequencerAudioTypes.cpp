// Copyright (c) Yevhenii Selivanov

#include "LevelSequencerAudioTypes.h"
//---
#include "LevelSequencerAudioSettings.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LevelSequencerAudioTypes)

/** Invalid trim times. */
const FTrimTimes FTrimTimes::Invalid = FTrimTimes{-1, -1, nullptr};

FTrimTimes::FTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs, USoundWave* InSoundWave)
	: StartTimeMs(InStartTimeMs), EndTimeMs(InEndTimeMs), SoundWave(InSoundWave) {}

// Returns true if the start and end times are valid.
bool FTrimTimes::IsValid() const
{
	return StartTimeMs >= 0 && EndTimeMs >= 0 && SoundWave != nullptr;
}

// Returns true if the start and end times are similar to the other trim times within the given tolerance.
bool FTrimTimes::IsSimilar(const FTrimTimes& Other, int32 ToleranceMs) const
{
	return SoundWave == Other.SoundWave &&
		FMath::Abs(StartTimeMs - Other.StartTimeMs) <= ToleranceMs &&
		FMath::Abs(EndTimeMs - Other.EndTimeMs) <= ToleranceMs;
}

// Equal operator for comparing in TMap.
bool FTrimTimes::operator==(const FTrimTimes& Other) const
{
	const int32 ToleranceMs = ULevelSequencerAudioSettings::Get().MinDifferenceMs;
	return IsSimilar(Other, ToleranceMs);
}

// Hash function to TMap
uint32 GetTypeHash(const FTrimTimes& TrimTimes)
{
	return GetTypeHash(TrimTimes.SoundWave) ^
		GetTypeHash(TrimTimes.StartTimeMs) ^
		GetTypeHash(TrimTimes.EndTimeMs);
}
