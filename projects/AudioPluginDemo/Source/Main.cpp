/*
  ==============================================================================

    This file was auto-generated and contains the startup code for a PIP.

  ==============================================================================
*/

// 1024 for filename and int for shell Id, 64 for category name, 64 for name and 16 for guid
char pgCommsMem[1024+4+64+64+16] __attribute__((used)) = "CADVSTMarker";
int  *pgChildID       = (int *)pgCommsMem;
char *pgCategoryName  = pgCommsMem+4;
const char *sgOrigVst = pgCommsMem+4+64;
const char *sgName    = pgCommsMem+4+64+1024;
char *pgGuid       = (char *)pgCommsMem+4+64+1024+64;



#include <JuceHeader.h>
#include "AudioPluginDemo.h"

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new JuceDemoPluginAudioProcessor();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter(juce::AudioProcessor::WrapperType type, void *pData)
{
  return new JuceDemoPluginAudioProcessor();
}
