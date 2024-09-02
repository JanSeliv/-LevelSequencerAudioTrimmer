// Copyright (c) Yevhenii Selivanov

#include "Data/LSATTrimTimesData.h"
//---
#include "LSATSettings.h"
//---
#include "LevelSequence.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sound/SoundWave.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LSATTrimTimesData)

/*********************************************************************************************
 * FLSATTrimTimes
 ********************************************************************************************* */

/** Invalid trim times. */
const FLSATTrimTimes FLSATTrimTimes::Invalid = FLSATTrimTimes{-1, -1};

FLSATTrimTimes::FLSATTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs)
	: StartTimeMs(InStartTimeMs), EndTimeMs(InEndTimeMs) {}

// Returns true if the audio section is looping (repeating playing from the start)
bool FLSATTrimTimes::IsLooping() const
{
	const int32 DifferenceMs = EndTimeMs - GetTotalDurationMs();
	return EndTimeMs > GetTotalDurationMs()
		&& DifferenceMs >= ULSATSettings::Get().MinDifferenceMs;
}

// Returns the total duration of the sound wave asset in milliseconds, it might be different from the actual usage duration
int32 FLSATTrimTimes::GetTotalDurationMs() const
{
	return SoundWave ? static_cast<int32>(SoundWave->Duration * 1000.0f) : 0;
}

// Returns true if usage duration and total duration are similar
bool FLSATTrimTimes::IsUsageSimilarToTotalDuration() const
{
	const int32 TotalDurationMs = GetTotalDurationMs();
	return TotalDurationMs - GetUsageDurationMs() < ULSATSettings::Get().MinDifferenceMs;
}

// Returns true if the start and end times are valid.
bool FLSATTrimTimes::IsValid() const
{
	return StartTimeMs >= 0 && EndTimeMs >= 0 && SoundWave != nullptr;
}

// Returns true if the start and end times are similar to the other trim times within the given tolerance.
bool FLSATTrimTimes::IsSimilar(const FLSATTrimTimes& Other, int32 ToleranceMs) const
{
	return SoundWave == Other.SoundWave &&
		FMath::Abs(StartTimeMs - Other.StartTimeMs) <= ToleranceMs &&
		FMath::Abs(EndTimeMs - Other.EndTimeMs) <= ToleranceMs;
}

// Equal operator for comparing in TMap.
bool FLSATTrimTimes::operator==(const FLSATTrimTimes& Other) const
{
	const int32 ToleranceMs = ULSATSettings::Get().MinDifferenceMs;
	return IsSimilar(Other, ToleranceMs);
}

// Hash function to TMap
uint32 GetTypeHash(const FLSATTrimTimes& TrimTimes)
{
	return GetTypeHash(TrimTimes.SoundWave) ^
		GetTypeHash(TrimTimes.StartTimeMs) ^
		GetTypeHash(TrimTimes.EndTimeMs);
}

/*********************************************************************************************
 * FLSATSectionsContainer
 ********************************************************************************************* */

bool FLSATSectionsContainer::Add(UMovieSceneAudioSection* AudioSection)
{
	return AudioSections.AddUnique(AudioSection) >= 0;
}

/*********************************************************************************************
 * FLSATTrimTimesMap
 ********************************************************************************************* */

bool FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection)
{
	return TrimTimesMap.FindOrAdd(TrimTimes).Add(AudioSection);
}

// Returns the first level sequence from the audio sections container
class ULevelSequence* FLSATTrimTimesMap::GetFirstLevelSequence() const
{
	const TArray<TObjectPtr<class UMovieSceneAudioSection>>* Sections = !TrimTimesMap.IsEmpty() ? &TrimTimesMap.CreateConstIterator()->Value.AudioSections : nullptr;
	const UMovieSceneAudioSection* Section = Sections && !Sections->IsEmpty() ? (*Sections)[0] : nullptr;
	return Section ? Section->GetTypedOuter<ULevelSequence>() : nullptr;
}
