﻿// Copyright (c) Yevhenii Selivanov

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
	return StartTimeMs >= 0
		&& EndTimeMs >= 0
		&& SoundWave != nullptr
		&& AudioSection != nullptr;
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

// Sets the sound wave for all audio sections in this container
void FLSATSectionsContainer::SetSound(USoundWave* SoundWave)
{
	for (UMovieSceneAudioSection* SectionIt : AudioSections)
	{
		if (SectionIt)
		{
			SectionIt->SetSound(SoundWave);
		}
	}
}

bool FLSATSectionsContainer::Add(UMovieSceneAudioSection* AudioSection)
{
	return AudioSections.AddUnique(AudioSection) >= 0;
}

/*********************************************************************************************
 * FLSATTrimTimesMap
 ********************************************************************************************* */

// Returns the first level sequence from the audio sections container
class ULevelSequence* FLSATTrimTimesMap::GetFirstLevelSequence() const
{
	const TArray<TObjectPtr<UMovieSceneAudioSection>>* Sections = !TrimTimesMap.IsEmpty() ? &TrimTimesMap.CreateConstIterator()->Value.AudioSections : nullptr;
	const UMovieSceneAudioSection* Section = Sections && !Sections->IsEmpty() ? (*Sections)[0] : nullptr;
	return Section ? Section->GetTypedOuter<ULevelSequence>() : nullptr;
}

// Sets the sound wave for all trim times in this map
void FLSATTrimTimesMap::SetSound(USoundWave* SoundWave)
{
	for (TTuple<FLSATTrimTimes, FLSATSectionsContainer>& ItRef : TrimTimesMap)
	{
		ItRef.Key.SoundWave = SoundWave;
		ItRef.Value.SetSound(SoundWave);
	}
}

bool FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection)
{
	return TrimTimesMap.FindOrAdd(TrimTimes).Add(AudioSection);
}

FLSATSectionsContainer& FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, const FLSATSectionsContainer& SectionsContainer)
{
	return TrimTimesMap.Add(TrimTimes, SectionsContainer);
}

/*********************************************************************************************
 * FLSATTrimTimesMultiMap
 ********************************************************************************************* */

// Returns all sounds waves from this multimap that satisfies the given predicate
void FLSATTrimTimesMultiMap::GetSounds(TArray<USoundWave*>& OutSoundWaves, TFunctionRef<bool(const TTuple<FLSATTrimTimes, FLSATSectionsContainer>&)> Predicate) const
{
	if (!OutSoundWaves.IsEmpty())
	{
		OutSoundWaves.Empty();
	}

	for (const TTuple<TObjectPtr<USoundWave>, FLSATTrimTimesMap>& OuterIt : TrimTimesMultiMap)
	{
		USoundWave* SoundWave = OuterIt.Key;
		const FLSATTrimTimesMap& TrimTimesMap = OuterIt.Value;
		if (!SoundWave)
		{
			continue;
		}

		for (const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& InnerPair : TrimTimesMap)
		{
			if (Predicate(InnerPair))
			{
				// Break inner map, go to the next sound wave (outer map)
				OutSoundWaves.AddUnique(SoundWave);
				break;
			}
		}
	}
}

void FLSATTrimTimesMultiMap::Remove(const TArray<USoundWave*>& SoundWaves)
{
	for (USoundWave* SoundWave : SoundWaves)
	{
		TrimTimesMultiMap.Remove(SoundWave);
	}
}
