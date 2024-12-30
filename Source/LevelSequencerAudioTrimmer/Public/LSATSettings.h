#pragma once

#include "Engine/DeveloperSettings.h"
//---
#include "Data/LSATPolicyTypes.h"
//---
#include "LSATSettings.generated.h"

/**
 * Developer Settings for the Level Sequencer Audio Trimmer plugin.
 * Is set up in 'Project Settings' -> "Plugins" -> "Level Sequencer Audio Trimmer".
 */
UCLASS(Config = LevelSequencerAudioTrimmer, DefaultConfig, DisplayName = "Level Sequencer Audio Trimmer")
class LEVELSEQUENCERAUDIOTRIMMERED_API ULSATSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Returns Project Settings Data of the Level Sequencer Audio Trimmer plugin. */
	static const FORCEINLINE ULSATSettings& Get() { return *GetDefault<ThisClass>(); }

	/** Gets the settings container name for the settings, either Project or Editor */
	virtual FName GetContainerName() const override { return TEXT("Project"); }

	/** Gets the category for the settings, some high level grouping like, Editor, Engine, Game...etc. */
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** Getter of the minimum difference between total audio duration and section usage. */ 
	static FORCEINLINE int32 GetMinDifferenceMs() { return FMath::Max(Get().MinDifferenceMs, 1); }

	/** Skip processing if the difference between total audio duration and section usage is less than this value.
	 * It shouldn't be less than 1 ms, as it might cause issues with the calculations. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "Audio Trimming", meta = (ClampMin = "1", UIMin = "1"))
	int32 MinDifferenceMs;

	/** Policy for handling the audio tracks that are looping meaning a sound is repeating playing from the start. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "Audio Trimming|Policy")
	ELSATPolicyLoopingSounds PolicyLoopingSounds;

	/** Policy for handling sound waves that are used outside of level sequences, such as in the world or blueprints.*/
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "Audio Trimming|Policy")
	ELSATPolicySoundsOutsideSequences PolicySoundsOutsideSequences;

	/** Policy for handling the audio tracks with different trim times for the same sound wave. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "Audio Trimming|Policy")
	ELSATPolicyDifferentTrimTimes PolicyDifferentTrimTimes;

	/** Defines policies for handling the reuse and fragmentation of sound segments within a level sequence.
	 * This policy control how overlapping sound usage is processed when trimming and reimporting sound assets. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "Audio Trimming|Policy")
	ELSATPolicyFragmentation PolicyFragmentation;
};
