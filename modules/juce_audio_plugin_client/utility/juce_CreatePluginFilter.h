/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   By using JUCE, you agree to the terms of both the JUCE 6 End-User License
   Agreement and JUCE Privacy Policy (both effective as of the 16th June 2020).

   End User License Agreement: www.juce.com/juce-6-licence
   Privacy Policy: www.juce.com/juce-privacy-policy

   Or: You may also use this code under the terms of the GPL v3 (see
   www.gnu.org/licenses).

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#pragma once

/** Somewhere in the codebase of your plugin, you need to implement this function
    and make it return a new instance of the filter subclass that you're building.
*/
// CAD Change START
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(juce::AudioProcessor::WrapperType type, void *pData);
// CAD Change END

namespace juce
{
// CAD Change START
inline AudioProcessor* JUCE_API JUCE_CALLTYPE createPluginFilterOfType (AudioProcessor::WrapperType type, void *pData = nullptr)
// CAD Change END
{
    AudioProcessor::setTypeOfNextNewPlugin (type);
	// CAD Change START
    AudioProcessor* const pluginInstance = ::createPluginFilter(type, pData);
    AudioProcessor::setTypeOfNextNewPlugin (AudioProcessor::wrapperType_Undefined);
	// CAD Change END
    // your createPluginFilter() method must return an object!
    jassert (pluginInstance != nullptr && pluginInstance->wrapperType == type);

    return pluginInstance;
}

} // namespace juce
