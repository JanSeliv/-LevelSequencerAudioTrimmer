// Copyright (c) Yevhenii Selivanov

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
};
