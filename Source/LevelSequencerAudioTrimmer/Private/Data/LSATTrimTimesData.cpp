// Copyright (c) Yevhenii Selivanov

#include "Data/LSATTrimTimesData.h"
//---
#include "LSATSettings.h"
#include "LSATUtilsLibrary.h"
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
const FLSATTrimTimes FLSATTrimTimes::Invalid = FLSATTrimTimes{-1, -1, nullptr};

FLSATTrimTimes::FLSATTrimTimes(const UMovieSceneAudioSection* AudioSection)
{
	const FFrameRate TickResolution = ULSATUtilsLibrary::GetTickResolution(AudioSection);
	if (!ensureMsgf(TickResolution.IsValid(), TEXT("ASSERT: [%i] %hs:\n'TickResolution' is not valid!"), __LINE__, __FUNCTION__))
	{
		*this = Invalid;
		return;
	}

	SoundWave = Cast<USoundWave>(AudioSection->GetSound());
	if (!ensureMsgf(SoundWave, TEXT("ASSERT: [%i] %hs:\n'SoundWave' is not valid!"), __LINE__, __FUNCTION__)
		|| !ensureMsgf(GetSoundTotalDurationMs() > 0.f, TEXT("ASSERT: [%i] %hs:\nDuration of '%s' sound is not valid!"), __LINE__, __FUNCTION__, *GetNameSafe(SoundWave)))
	{
		*this = Invalid;
		return;
	}

	// Get the audio start offset in frames (relative to the audio asset)
	SoundTrimStartMs = ULSATUtilsLibrary::ConvertFrameToMs(AudioSection->GetStartOffset(), TickResolution);

	// Calculate the effective end time within the audio asset
	const int32 SectionStartFrame = ULSATUtilsLibrary::GetSectionInclusiveStartTimeMs(AudioSection);
	const int32 SectionEndMs = ULSATUtilsLibrary::GetSectionExclusiveEndTimeMs(AudioSection);
	const int32 SectionDurationMs = SectionEndMs - SectionStartFrame;
	SoundTrimEndMs = GetSoundTrimStartMs() + SectionDurationMs;
}

FLSATTrimTimes::FLSATTrimTimes(int32 InSoundTrimStartMs, int32 InSoundTrimEndMs, USoundWave* InSoundWave)
	: SoundTrimStartMs(InSoundTrimStartMs), SoundTrimEndMs(InSoundTrimEndMs), SoundWave(InSoundWave) {}

/*********************************************************************************************
 * Trim Times Methods
 ********************************************************************************************* */

// Returns SoundTrimStartMs in frames based on the tick resolution, or -1 if cannot convert
int32 FLSATTrimTimes::GetSoundTrimStartFrame(const struct FFrameRate& TickResolution) const
{
	return ULSATUtilsLibrary::ConvertMsToFrame(SoundTrimStartMs, TickResolution);
}

// Returns SoundTrimEndMs in frames based on the tick resolution, or -1 if cannot convert
int32 FLSATTrimTimes::GetSoundTrimEndFrame(const struct FFrameRate& TickResolution) const
{
	return ULSATUtilsLibrary::ConvertMsToFrame(SoundTrimEndMs, TickResolution);
}

// Returns true if the audio section is looping (repeating playing from the start)
bool FLSATTrimTimes::IsLooping() const
{
	const int32 DifferenceMs = SoundTrimEndMs - GetSoundTotalDurationMs();
	return SoundTrimEndMs > GetSoundTotalDurationMs()
		&& DifferenceMs >= ULSATSettings::Get().MinDifferenceMs;
}

// Returns the usage percentage of the sound wave asset in 0-100 range
float FLSATTrimTimes::GetUsagePercentage() const
{
	if (IsSoundTrimmed())
	{
		return 100.f;
	}

	if (GetSoundTotalDurationMs() <= 0)
	{
		return 0.f;
	}

	const float InUsageDurationMs = static_cast<float>(GetUsageDurationMs());
	const float InTotalDurationMs = static_cast<float>(GetSoundTotalDurationMs());
	return (InUsageDurationMs / InTotalDurationMs) * 100.f;
}

// Returns the number of frames the sound wave asset is used
int32 FLSATTrimTimes::GetUsagesFrames(const FFrameRate& TickResolution) const
{
	return ULSATUtilsLibrary::ConvertMsToFrame(GetUsageDurationMs(), TickResolution);
}

// Returns the total duration of the sound wave asset in milliseconds, it might be different from the actual usage duration
int32 FLSATTrimTimes::GetSoundTotalDurationMs() const
{
	return SoundWave ? FMath::CeilToInt(SoundWave->Duration * 1000.f) : 0;
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

// Returns true if duration is valid and positive
bool FLSATTrimTimes::IsValidLength(const FFrameRate& TickResolution) const
{
	if (!IsValid()
		|| !TickResolution.IsValid()
		|| GetUsageDurationMs() < ULSATSettings::Get().MinDifferenceMs)
	{
		return false;
	}

	const FFrameNumber SectionStartFrame = ULSATUtilsLibrary::ConvertMsToFrameNumber(SoundTrimStartMs, TickResolution);
	const FFrameNumber SectionEndFrame = ULSATUtilsLibrary::ConvertMsToFrameNumber(SoundTrimEndMs, TickResolution);
	return SectionStartFrame < SectionEndFrame;
}

// Checks if the trim times are within the bounds of the given audio section
bool FLSATTrimTimes::IsWithinSectionBounds(const UMovieSceneAudioSection* AudioSection) const
{
	const int32 SectionStartMs = ULSATUtilsLibrary::GetSectionInclusiveStartTimeMs(AudioSection);
	const int32 SectionEndMs = ULSATUtilsLibrary::GetSectionExclusiveEndTimeMs(AudioSection);
	return SoundTrimStartMs >= SectionStartMs
		&& SoundTrimEndMs <= SectionEndMs;
}

// Checks if the trim times are within the original trim times
bool FLSATTrimTimes::IsWithinTrimBounds(const FLSATTrimTimes& OtherTrimTimes) const
{
	return SoundTrimStartMs >= OtherTrimTimes.SoundTrimStartMs
		&& SoundTrimEndMs <= OtherTrimTimes.SoundTrimEndMs;
}

// Returns larger mix of the two trim times: larger start time from both and larger end time from both
FLSATTrimTimes FLSATTrimTimes::GetMaxTrimTimes(const FLSATTrimTimes& Left, const FLSATTrimTimes& Right)
{
	return FLSATTrimTimes{
		FMath::Max(Left.SoundTrimStartMs, Right.SoundTrimStartMs),
		FMath::Max(Left.SoundTrimEndMs, Right.SoundTrimEndMs),
		Left.SoundWave
	};
}

// Returns the string representation of the trim times that might be useful for logging
FString FLSATTrimTimes::ToString(const FFrameRate& TickResolution) const
{
	return FString::Printf(TEXT("Audio: %s "
		"| Usage: %d ms (frame %d) to %d ms (frame %d) "
		"| Duration: %.2f sec (%d frames) "
		"| Percentage Used: %1.f%%"),
	                       *SoundWave->GetName()
	                       , SoundTrimStartMs, GetSoundTrimStartFrame(TickResolution), SoundTrimEndMs, GetSoundTrimEndFrame(TickResolution)
	                       , GetSoundTrimEndSeconds() - GetSoundTrimStartSeconds(), GetUsagesFrames(TickResolution)
	                       , GetUsagePercentage());
}

// Returns short string representation of the trim times
FString FLSATTrimTimes::ToCompactString() const
{
	return FString::Printf(TEXT("SoundWave: %s | SoundTrimStartMs: %d | SoundTrimEndMs: %d"),
	                       *GetNameSafe(SoundWave), SoundTrimStartMs, SoundTrimEndMs);
}

/*********************************************************************************************
 * Trim Times Operators
 ********************************************************************************************* */

// Equal operator for comparing in TMap.
bool FLSATTrimTimes::operator==(const FLSATTrimTimes& Other) const
{
	return GetTypeHash(*this) == GetTypeHash(Other);
}

// Hash function to TMap
uint32 GetTypeHash(const FLSATTrimTimes& TrimTimes)
{
	const int32 ToleranceMs = ULSATSettings::Get().MinDifferenceMs;

	const int32 StartDivided = TrimTimes.SoundTrimStartMs / ToleranceMs;
	const int32 EndDivided = TrimTimes.SoundTrimEndMs / ToleranceMs;

	const int32 RoundedStart = StartDivided * ToleranceMs;
	const int32 RoundedEnd = EndDivided * ToleranceMs;

	return GetTypeHash(RoundedStart)
		^ GetTypeHash(RoundedEnd)
		^ GetTypeHash(TrimTimes.SoundWave);
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

void FLSATSectionsContainer::Append(const FLSATSectionsContainer& Other)
{
	for (UMovieSceneAudioSection* SectionIt : Other)
	{
		if (SectionIt)
		{
			AudioSections.AddUnique(SectionIt);
		}
	}
}

/*********************************************************************************************
 * FLSATTrimTimesMap
 ********************************************************************************************* */

// Returns first audio section from the audio sections container
const UMovieSceneAudioSection* FLSATTrimTimesMap::GetFirstAudioSection() const
{
	const FLSATSectionsContainer* Sections = !TrimTimesMap.IsEmpty() ? &TrimTimesMap.CreateConstIterator()->Value : nullptr;
	if (!Sections || Sections->IsEmpty())
	{
		return nullptr;
	}

	for (const UMovieSceneAudioSection* AudioSection : *Sections)
	{
		if (AudioSection)
		{
			return AudioSection;
		}
	}

	return nullptr;
}

// Sets the sound wave for all trim times in this map
void FLSATTrimTimesMap::SetSound(USoundWave* SoundWave)
{
	for (TTuple<FLSATTrimTimes, FLSATSectionsContainer>& ItRef : TrimTimesMap)
	{
		ItRef.Key.SetSoundWave(SoundWave);
		ItRef.Value.SetSound(SoundWave);
	}
}

// Processes each audio section in this map and rebuilds the map if new sections are created
void FLSATTrimTimesMap::RebuildTrimTimesMapWithProcessor(const FLSATSectionsProcessor& Processor)
{
	FLSATSectionsContainer AllNewSections;
	TArray<FLSATTrimTimes> TrimTimesToRemove;

	for (const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& TrimTimesEntry : TrimTimesMap)
	{
		const FLSATTrimTimes& TrimTimes = TrimTimesEntry.Key;

		if (!TrimTimes.IsValid())
		{
			continue;
		}

		// Store the size of AllNewSections before processing
		const int32 BeforeSize = AllNewSections.Num();

		for (UMovieSceneAudioSection* AudioSection : TrimTimesEntry.Value)
		{
			Processor(AudioSection, TrimTimes, AllNewSections);
		}

		// If new sections were added, mark this TrimTimes for removal
		if (AllNewSections.Num() > BeforeSize)
		{
			TrimTimesToRemove.AddUnique(TrimTimes);
		}
	}

	// Only rebuild the map if new sections were added
	if (!TrimTimesToRemove.IsEmpty())
	{
		// Remove the original TrimTimes that have been processed
		for (const FLSATTrimTimes& TrimTimesToRemoveEntry : TrimTimesToRemove)
		{
			TrimTimesMap.Remove(TrimTimesToRemoveEntry);
		}

		// Recalculate and merge the new TrimTimes with their associated sections
		ULSATUtilsLibrary::CalculateTrimTimesInAllSections(*this, AllNewSections);

		SortKeys();
	}
}

bool FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, UMovieSceneAudioSection* AudioSection)
{
	if (!ensureMsgf(AudioSection, TEXT("ASSERT: [%i] %hs:\n'AudioSection' is not valid!"), __LINE__, __FUNCTION__)
		|| !ensureMsgf(TrimTimes.IsValid(), TEXT("ASSERT: [%i] %hs:\n'TrimTimes' is not valid!"), __LINE__, __FUNCTION__))
	{
		return false;
	}

	for (TTuple<FLSATTrimTimes, FLSATSectionsContainer>& ItRef : TrimTimesMap)
	{
		FLSATTrimTimes& TrimTimesRef = ItRef.Key;
		if (TrimTimesRef == TrimTimes)
		{
			// Assign the larger trim times as it might be not the same due to MinDifferenceMs
			TrimTimesRef = FLSATTrimTimes::GetMaxTrimTimes(TrimTimes, TrimTimesRef);

			return ItRef.Value.Add(AudioSection);
		}
	}

	// No Trim Times found, add a new one
	return TrimTimesMap.FindOrAdd(TrimTimes).Add(AudioSection);
}

FLSATSectionsContainer& FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes)
{
	return TrimTimesMap.Add(TrimTimes);
}

FLSATSectionsContainer& FLSATTrimTimesMap::Add(const FLSATTrimTimes& TrimTimes, const FLSATSectionsContainer& SectionsContainer)
{
	return TrimTimesMap.Add(TrimTimes, SectionsContainer);
}

void FLSATTrimTimesMap::SortKeys()
{
	TArray<TPair<FLSATTrimTimes, FLSATSectionsContainer>> TrimTimesArray;
	TrimTimesArray.Reserve(TrimTimesMap.Num());
	for (const TTuple<FLSATTrimTimes, FLSATSectionsContainer>& It : TrimTimesMap)
	{
		TrimTimesArray.Add(It);
	}

	// Sort the array by SoundTrimStartMs first, then by SoundTrimEndMs
	TrimTimesArray.Sort([](const TPair<FLSATTrimTimes, FLSATSectionsContainer>& A, const TPair<FLSATTrimTimes, FLSATSectionsContainer>& B)
	{
		if (A.Key.GetSoundTrimStartMs() != B.Key.GetSoundTrimStartMs())
		{
			return A.Key.GetSoundTrimStartMs() < B.Key.GetSoundTrimStartMs();
		}
		return A.Key.GetSoundTrimEndMs() < B.Key.GetSoundTrimEndMs();
	});

	// Clear the original map and re-populate it with the sorted data
	TrimTimesMap.Reset();
	for (const TPair<FLSATTrimTimes, FLSATSectionsContainer>& It : TrimTimesArray)
	{
		TrimTimesMap.Add(It);
	}
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
