// Copyright (c) Yevhenii Selivanov

#include "Data/LSATTrimTimesData.h"
//---
#include "LevelSequence.h"
#include "LSATSettings.h"
#include "Sections/MovieSceneAudioSection.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LSATTrimTimesData)

/** Invalid trim times. */
const FLSATTrimTimes FLSATTrimTimes::Invalid = FLSATTrimTimes{-1, -1, nullptr};

FLSATTrimTimes::FLSATTrimTimes(int32 InStartTimeMs, int32 InEndTimeMs, USoundWave* InSoundWave)
	: StartTimeMs(InStartTimeMs), EndTimeMs(InEndTimeMs), SoundWave(InSoundWave) {}

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

bool FLSATSectionsContainer::Add(UMovieSceneAudioSection* AudioSection)
{
	return AudioSections.AddUnique(AudioSection) >= 0;
}

// Hash function to TMap
uint32 GetTypeHash(const FLSATTrimTimes& TrimTimes)
{
	return GetTypeHash(TrimTimes.SoundWave) ^
		GetTypeHash(TrimTimes.StartTimeMs) ^
		GetTypeHash(TrimTimes.EndTimeMs);
}

// Returns the first level sequence from the audio sections container
class ULevelSequence* FLSATTrimTimesMap::GetFirstLevelSequence() const
{
	const TArray<TObjectPtr<class UMovieSceneAudioSection>>* Sections = !TrimTimesMap.IsEmpty() ? &TrimTimesMap.CreateConstIterator()->Value.AudioSections : nullptr;
	const UMovieSceneAudioSection* Section = !Sections->IsEmpty() ? (*Sections)[0] : nullptr;
	return Section ? Section->GetTypedOuter<ULevelSequence>() : nullptr;
}

bool FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection)
{
	return TrimTimesMap.FindOrAdd(TrimTimes).Add(AudioSection);
}
