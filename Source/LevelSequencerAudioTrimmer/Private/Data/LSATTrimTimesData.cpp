// Copyright (c) Yevhenii Selivanov

#include "Data/LSATTrimTimesData.h"
//---
#include "LSATSettings.h"
//---
#include "LevelSequence.h"
#include "MovieScene.h"
#include "Sections/MovieSceneAudioSection.h"
#include "Sound/SoundWave.h"
//---
#include UE_INLINE_GENERATED_CPP_BY_NAME(LSATTrimTimesData)

/*********************************************************************************************
 * FLSATTrimTimes
 ********************************************************************************************* */

/** Invalid trim times. */
const FLSATTrimTimes FLSATTrimTimes::Invalid = FLSATTrimTimes{-1, -1};

FLSATTrimTimes::FLSATTrimTimes(int32 InSoundTrimStartMs, int32 InSoundTrimEndMs)
	: SoundTrimStartMs(InSoundTrimStartMs), SoundTrimEndMs(InSoundTrimEndMs) {}

// Returns true if the audio section is looping (repeating playing from the start)
bool FLSATTrimTimes::IsLooping() const
{
	const int32 DifferenceMs = SoundTrimEndMs - GetSoundTotalDurationMs();
	return SoundTrimEndMs > GetSoundTotalDurationMs()
		&& DifferenceMs >= ULSATSettings::Get().MinDifferenceMs;
}

// Returns the total duration of the sound wave asset in milliseconds, it might be different from the actual usage duration
int32 FLSATTrimTimes::GetSoundTotalDurationMs() const
{
	return SoundWave ? static_cast<int32>(SoundWave->Duration * 1000.0f) : 0;
}

// Returns the actual start time of the audio section in the level Sequence in milliseconds
int32 FLSATTrimTimes::GetSectionInclusiveStartTimeMs(const UMovieSceneAudioSection* InAudioSection)
{
	if (!InAudioSection)
	{
		return INDEX_NONE;
	}

	const ULevelSequence* LevelSequence = InAudioSection->GetTypedOuter<ULevelSequence>();
	checkf(LevelSequence, TEXT("ERROR: [%i] %hs:\n'LevelSequence' is null!"), __LINE__, __FUNCTION__);
	const FFrameRate TickResolution = LevelSequence->GetMovieScene()->GetTickResolution();

	const FFrameNumber SectionStartFrame = InAudioSection->GetInclusiveStartFrame();
	return FMath::RoundToInt((SectionStartFrame.Value / TickResolution.AsDecimal()) * 1000.0f);
}

// Returns the actual end time of the audio section in the level Sequence in milliseconds
int32 FLSATTrimTimes::GetSectionExclusiveEndTimeMs(const UMovieSceneAudioSection* InAudioSection)
{
	if (!InAudioSection)
	{
		return INDEX_NONE;
	}

	const ULevelSequence* LevelSequence = InAudioSection->GetTypedOuter<ULevelSequence>();
	checkf(LevelSequence, TEXT("ERROR: [%i] %hs:\n'LevelSequence' is null!"), __LINE__, __FUNCTION__);
	const FFrameRate TickResolution = LevelSequence->GetMovieScene()->GetTickResolution();

	const FFrameNumber SectionEndFrame = InAudioSection->GetExclusiveEndFrame();
	return FMath::RoundToInt((SectionEndFrame.Value / TickResolution.AsDecimal()) * 1000.0f);
}

// Returns true if the sound is already trimmer, so usage duration and total duration are similar
bool FLSATTrimTimes::IsSoundTrimmed() const
{
	const int32 MinDifferenceMs = ULSATSettings::Get().MinDifferenceMs;
	const int32 DifferenceMs = GetSoundTotalDurationMs() - GetUsageDurationMs();
	return DifferenceMs < MinDifferenceMs
		&& SoundTrimStartMs < MinDifferenceMs;
}

// Returns true if the start and end times are valid.
bool FLSATTrimTimes::IsValid() const
{
	return SoundTrimStartMs >= 0
		&& SoundTrimEndMs >= 0
		&& SoundWave != nullptr;
}

// Returns true if the start and end times are similar to the other trim times within the given tolerance.
bool FLSATTrimTimes::IsSimilar(const FLSATTrimTimes& Other, int32 ToleranceMs) const
{
	return SoundWave == Other.SoundWave &&
		FMath::Abs(SoundTrimStartMs - Other.SoundTrimStartMs) <= ToleranceMs &&
		FMath::Abs(SoundTrimEndMs - Other.SoundTrimEndMs) <= ToleranceMs;
}

// Returns the string representation of the trim times that might be useful for logging
FString FLSATTrimTimes::ToString() const
{
	return FString::Printf(TEXT("SoundWave: %s | SoundTrimStartMs: %d | SoundTrimEndMs: %d"),
	                       *GetNameSafe(SoundWave), SoundTrimStartMs, SoundTrimEndMs);
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
		GetTypeHash(TrimTimes.SoundTrimStartMs) ^
		GetTypeHash(TrimTimes.SoundTrimEndMs);
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
void FLSATTrimTimesMultiMap::GetSounds(TArray<USoundWave*>& OutSoundWaves, const TFunctionRef<bool(const TTuple<FLSATTrimTimes, FLSATSectionsContainer>&)>& Predicate) const
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
