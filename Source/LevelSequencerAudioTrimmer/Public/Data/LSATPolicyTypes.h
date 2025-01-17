﻿// Copyright (c) Yevhenii Selivanov

#pragma once

#include "LSATPolicyTypes.generated.h"

/**
 * Policy for handling the audio tracks that are looping meaning a sound is repeating playing from the start.
 * This policy might be expanded in the future containing more options like merging the looping sounds into one sound.
 */
UENUM(BlueprintType)
enum class ELSATPolicyLoopingSounds : uint8
{
	///< This sound wave will not be processed at all for this and all other audio tracks that use the same sound wave. 
	SkipAll,
	///< Section with looping sound will not be processed, but all other usages of the same sound wave will be duplicated into separate sound wave asset.
	SkipAndDuplicate,
	///< Splits looping section into multiple segments at the points where the sound starts from beginning.
	SplitSections,
};

/**
 * Policy for handling sound waves that are used outside of level sequences, such as in the world or blueprints.
 * This policy might be expanded in the future with more options like use trimmed sound waves for external usage.
 */
UENUM(BlueprintType)
enum class ELSATPolicySoundsOutsideSequences : uint8
{
	///< This sound wave will not be processed at all if it's used anywhere outside level sequences.
	SkipAll,
	///< Sound wave used outside level sequences will not be touched, but those that used in level sequences will be duplicated.
	SkipAndDuplicate,
};

/**
 * Policy for handling the audio tracks with different trim times for the same sound wave.
 */
UENUM(BlueprintType)
enum class ELSATPolicyDifferentTrimTimes : uint8
{
	///< Skip processing for all tracks of this sound if it has different trim times.
	SkipAll,
	///< Duplicate sound wave asset for different trim times, but reimport only one of them.
	ReimportOneAndDuplicateOthers,
};

/**
* Defines policies for handling the reuse and fragmentation of sound segments within a level sequence.
* This policy control how overlapping sound usage is processed when trimming and reimporting sound assets.
*/
UENUM(BlueprintType)
enum class ELSATPolicyFragmentation : uint8
{
	///< Segments will not be fragmented and reused, but kept as original.
	None,
	///< Segments will be fragmented into smaller reusable parts, with each usage sharing overlapping segments.
	SplitToSmaller UMETA(DisplayName = "[EXPERIMENTAL] Split to Smaller"),
};
