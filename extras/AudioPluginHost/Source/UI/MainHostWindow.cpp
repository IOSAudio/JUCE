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

#include <JuceHeader.h>
#include "MainHostWindow.h"
#include "../Plugins/InternalPlugins.h"

constexpr const char* scanModeKey = "pluginScanMode";

class CustomPluginScanner  : public KnownPluginList::CustomScanner,
                             private ChangeListener
{
public:
    CustomPluginScanner()
    {
        if (auto* file = getAppProperties().getUserSettings())
            file->addChangeListener (this);

        changeListenerCallback (nullptr);
    }

    ~CustomPluginScanner() override
    {
        if (auto* file = getAppProperties().getUserSettings())
            file->removeChangeListener (this);
    }

    bool findPluginTypesFor (AudioPluginFormat& format,
                             OwnedArray<PluginDescription>& result,
                             const String& fileOrIdentifier) override
    {
        if (scanInProcess)
        {
            superprocess = nullptr;
            format.findAllTypesForFile (result, fileOrIdentifier);
            return true;
        }

        if (superprocess == nullptr)
        {
            superprocess = std::make_unique<Superprocess> (*this);

            std::unique_lock<std::mutex> lock (mutex);
            connectionLost = false;
        }

        MemoryBlock block;
        MemoryOutputStream stream { block, true };
        stream.writeString (format.getName());
        stream.writeString (fileOrIdentifier);

        if (superprocess->sendMessageToWorker (block))
        {
            std::unique_lock<std::mutex> lock (mutex);
            gotResponse = false;
            pluginDescription = nullptr;

            for (;;)
            {
                if (condvar.wait_for (lock,
                                      std::chrono::milliseconds (50),
                                      [this] { return gotResponse || shouldExit(); }))
                {
                    break;
                }
            }

            if (shouldExit())
            {
                superprocess = nullptr;
                return true;
            }

            if (connectionLost)
            {
                superprocess = nullptr;
                return false;
            }

            if (pluginDescription != nullptr)
            {
                for (const auto* item : pluginDescription->getChildIterator())
                {
                    auto desc = std::make_unique<PluginDescription>();

                    if (desc->loadFromXml (*item))
                        result.add (std::move (desc));
                }
            }

            return true;
        }

        superprocess = nullptr;
        return false;
    }

    void scanFinished() override
    {
        superprocess = nullptr;
    }

private:
    class Superprocess  : private ChildProcessCoordinator
    {
    public:
        explicit Superprocess (CustomPluginScanner& o)
            : owner (o)
        {
            launchWorkerProcess (File::getSpecialLocation (File::currentExecutableFile), processUID, 0, 0);
        }

        using ChildProcessCoordinator::sendMessageToWorker;

    private:
        void handleMessageFromWorker (const MemoryBlock& mb) override
        {
            auto xml = parseXML (mb.toString());

            const std::lock_guard<std::mutex> lock (owner.mutex);
            owner.pluginDescription = std::move (xml);
            owner.gotResponse = true;
            owner.condvar.notify_one();
        }

        void handleConnectionLost() override
        {
            const std::lock_guard<std::mutex> lock (owner.mutex);
            owner.pluginDescription = nullptr;
            owner.gotResponse = true;
            owner.connectionLost = true;
            owner.condvar.notify_one();
        }

        CustomPluginScanner& owner;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Superprocess)
    };

    void changeListenerCallback (ChangeBroadcaster*) override
    {
        if (auto* file = getAppProperties().getUserSettings())
            scanInProcess = (file->getIntValue (scanModeKey) == 0);
    }

    std::unique_ptr<Superprocess> superprocess;
    std::mutex mutex;
    std::condition_variable condvar;
    std::unique_ptr<XmlElement> pluginDescription;
    bool gotResponse = false;
    bool connectionLost = false;

    std::atomic<bool> scanInProcess { true };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomPluginScanner)
};

//==============================================================================
class CustomPluginListComponent  : public PluginListComponent
{
public:
    CustomPluginListComponent (AudioPluginFormatManager& manager,
                               KnownPluginList& listToRepresent,
                               const File& pedal,
                               PropertiesFile* props,
                               bool async)
        : PluginListComponent (manager, listToRepresent, pedal, props, async)
    {
        addAndMakeVisible (validationModeLabel);
        addAndMakeVisible (validationModeBox);

        validationModeLabel.attachToComponent (&validationModeBox, true);
        validationModeLabel.setJustificationType (Justification::right);
        validationModeLabel.setSize (100, 30);

        auto unusedId = 1;

        for (const auto mode : { "In-process", "Out-of-process" })
            validationModeBox.addItem (mode, unusedId++);

        validationModeBox.setSelectedItemIndex (getAppProperties().getUserSettings()->getIntValue (scanModeKey));

        validationModeBox.onChange = [this]
        {
            getAppProperties().getUserSettings()->setValue (scanModeKey, validationModeBox.getSelectedItemIndex());
        };

        resized();
    }

    void resized() override
    {
        PluginListComponent::resized();

        const auto& buttonBounds = getOptionsButton().getBounds();
        validationModeBox.setBounds (buttonBounds.withWidth (130).withRightX (getWidth() - buttonBounds.getX()));
    }

private:
    Label validationModeLabel { {}, "Scan mode" };
    ComboBox validationModeBox;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomPluginListComponent)
};

//==============================================================================
class MainHostWindow::PluginListWindow  : public DocumentWindow
{
public:
    PluginListWindow (MainHostWindow& mw, AudioPluginFormatManager& pluginFormatManager)
        : DocumentWindow ("Available Plugins",
                          LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          owner (mw)
    {
        auto deadMansPedalFile = getAppProperties().getUserSettings()
                                   ->getFile().getSiblingFile ("RecentlyCrashedPluginsList");
      
        bool bAllowAsync = false; // if true hardly any vst3 plugins validate because juce is creating them and killing them in a thread, unfortunetaly there are still messages for them on the main thread. bang

        setContentOwned (new PluginListComponent (pluginFormatManager,
                                                  owner.knownPluginList,
                                                  deadMansPedalFile,
                                                  getAppProperties().getUserSettings(), bAllowAsync), true);

        setResizable (true, false);
		// CAD Change START
        setResizeLimits (300, 400, 10000, 10000);
		// CAD Change END
        setTopLeftPosition (60, 60);

        restoreWindowStateFromString (getAppProperties().getUserSettings()->getValue ("listWindowPos"));
        setVisible (true);
    }

    ~PluginListWindow() override
    {
        getAppProperties().getUserSettings()->setValue ("listWindowPos", getWindowStateAsString());
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        owner.pluginListWindow = nullptr;
    }

private:
    MainHostWindow& owner;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginListWindow)
};

//==============================================================================
MainHostWindow::MainHostWindow()
    : DocumentWindow (JUCEApplication::getInstance()->getApplicationName(),
                      LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                      DocumentWindow::allButtons)
{
    formatManager.addDefaultFormats();
    formatManager.addFormat (new InternalPluginFormat());

    auto safeThis = SafePointer<MainHostWindow> (this);
    RuntimePermissions::request (RuntimePermissions::recordAudio,
                                 [safeThis] (bool granted) mutable
                                 {
                                     auto savedState = getAppProperties().getUserSettings()->getXmlValue ("audioDeviceState");
                                     safeThis->deviceManager.initialise (granted ? 256 : 0, 256, savedState.get(), true);
                                 });

   #if JUCE_IOS || JUCE_ANDROID
    setFullScreen (true);
   #else
    setResizable (true, false);
    setResizeLimits (500, 400, 10000, 10000);
	// CAD Change START
    centreWithSize (1500, 1500);
	// CAD Change END
   #endif

    knownPluginList.setCustomScanner (std::make_unique<CustomPluginScanner>());

    graphHolder.reset (new GraphDocumentComponent (formatManager, deviceManager, knownPluginList));

    setContentNonOwned (graphHolder.get(), false);

    setUsingNativeTitleBar (true);

    restoreWindowStateFromString (getAppProperties().getUserSettings()->getValue ("mainWindowPos"));

    setVisible (true);

    InternalPluginFormat internalFormat;
    internalTypes = internalFormat.getAllTypes();

    if (auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue ("pluginList"))
        knownPluginList.recreateFromXml (*savedPluginList);

    for (auto& t : internalTypes)
        knownPluginList.addType (t);

    pluginSortMethod = (KnownPluginList::SortMethod) getAppProperties().getUserSettings()
                            ->getIntValue ("pluginSortMethod", KnownPluginList::sortByManufacturer);

    knownPluginList.addChangeListener (this);

    if (auto* g = graphHolder->graph.get())
        g->addChangeListener (this);

    addKeyListener (getCommandManager().getKeyMappings());

    Process::setPriority (Process::HighPriority);

  #if JUCE_IOS || JUCE_ANDROID
    graphHolder->burgerMenu.setModel (this);
  #else
   #if JUCE_MAC
    setMacMainMenu (this);
   #else
    setMenuBar (this);
   #endif
  #endif

    getCommandManager().setFirstCommandTarget (this);
}

MainHostWindow::~MainHostWindow()
{
    pluginListWindow = nullptr;
    knownPluginList.removeChangeListener (this);

    if (auto* g = graphHolder->graph.get())
        g->removeChangeListener (this);

    getAppProperties().getUserSettings()->setValue ("mainWindowPos", getWindowStateAsString());
    clearContentComponent();

  #if ! (JUCE_ANDROID || JUCE_IOS)
   #if JUCE_MAC
    setMacMainMenu (nullptr);
   #else
    setMenuBar (nullptr);
   #endif
  #endif

    graphHolder = nullptr;
}

void MainHostWindow::closeButtonPressed()
{
    tryToQuitApplication();
}

struct AsyncQuitRetrier  : private Timer
{
    AsyncQuitRetrier()   { startTimer (500); }

    void timerCallback() override
    {
        stopTimer();
        delete this;

        if (auto app = JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
    }
};

void MainHostWindow::tryToQuitApplication()
{
    if (graphHolder->closeAnyOpenPluginWindows())
    {
        // Really important thing to note here: if the last call just deleted any plugin windows,
        // we won't exit immediately - instead we'll use our AsyncQuitRetrier to let the message
        // loop run for another brief moment, then try again. This will give any plugins a chance
        // to flush any GUI events that may have been in transit before the app forces them to
        // be unloaded
        new AsyncQuitRetrier();
        return;
    }

    if (ModalComponentManager::getInstance()->cancelAllModalComponents())
    {
        new AsyncQuitRetrier();
        return;
    }

    if (graphHolder != nullptr)
    {
        auto releaseAndQuit = [this]
        {
            // Some plug-ins do not want [NSApp stop] to be called
            // before the plug-ins are not deallocated.
            graphHolder->releaseGraph();

            JUCEApplication::quit();
        };

       #if JUCE_ANDROID || JUCE_IOS
        if (graphHolder->graph->saveDocument (PluginGraph::getDefaultGraphDocumentOnMobile()))
            releaseAndQuit();
       #else
        SafePointer<MainHostWindow> parent { this };
        graphHolder->graph->saveIfNeededAndUserAgreesAsync ([parent, releaseAndQuit] (FileBasedDocument::SaveResult r)
        {
            if (parent == nullptr)
                return;

            if (r == FileBasedDocument::savedOk)
                releaseAndQuit();
        });
       #endif

        return;
    }

    JUCEApplication::quit();
}

void MainHostWindow::changeListenerCallback (ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        menuItemsChanged();

        // save the plugin list every time it gets changed, so that if we're scanning
        // and it crashes, we've still saved the previous ones
        if (auto savedPluginList = std::unique_ptr<XmlElement> (knownPluginList.createXml()))
        {
            getAppProperties().getUserSettings()->setValue ("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
    else if (graphHolder != nullptr && changed == graphHolder->graph.get())
    {
        auto title = JUCEApplication::getInstance()->getApplicationName();
        auto f = graphHolder->graph->getFile();

        if (f.existsAsFile())
            title = f.getFileName() + " - " + title;

        setName (title);
    }
}

StringArray MainHostWindow::getMenuBarNames()
{
    StringArray names;
    names.add ("File");
    names.add ("Plugins");
    names.add ("Options");
    names.add ("Windows");
    return names;
}

PopupMenu MainHostWindow::getMenuForIndex (int topLevelMenuIndex, const String& /*menuName*/)
{
    PopupMenu menu;

    if (topLevelMenuIndex == 0)
    {
        // "File" menu
       #if ! (JUCE_IOS || JUCE_ANDROID)
        menu.addCommandItem (&getCommandManager(), CommandIDs::newFile);
        menu.addCommandItem (&getCommandManager(), CommandIDs::open);
       #endif

        RecentlyOpenedFilesList recentFiles;
        recentFiles.restoreFromString (getAppProperties().getUserSettings()
                                            ->getValue ("recentFilterGraphFiles"));

        PopupMenu recentFilesMenu;
        recentFiles.createPopupMenuItems (recentFilesMenu, 100, true, true);
        menu.addSubMenu ("Open recent file", recentFilesMenu);

       #if ! (JUCE_IOS || JUCE_ANDROID)
        menu.addCommandItem (&getCommandManager(), CommandIDs::save);
        menu.addCommandItem (&getCommandManager(), CommandIDs::saveAs);
       #endif

        menu.addSeparator();
        menu.addCommandItem (&getCommandManager(), StandardApplicationCommandIDs::quit);
    }
    else if (topLevelMenuIndex == 1)
    {
        // "Plugins" menu
        PopupMenu pluginsMenu;
        addPluginsToMenu (pluginsMenu);
        menu.addSubMenu ("Create Plug-in", pluginsMenu);
        menu.addSeparator();
        menu.addItem (250, "Delete All Plug-ins");
    }
    else if (topLevelMenuIndex == 2)
    {
        // "Options" menu

        menu.addCommandItem (&getCommandManager(), CommandIDs::showPluginListEditor);

        PopupMenu sortTypeMenu;
        sortTypeMenu.addItem (200, "List Plug-ins in Default Order",      true, pluginSortMethod == KnownPluginList::defaultOrder);
        sortTypeMenu.addItem (201, "List Plug-ins in Alphabetical Order", true, pluginSortMethod == KnownPluginList::sortAlphabetically);
        sortTypeMenu.addItem (202, "List Plug-ins by Category",           true, pluginSortMethod == KnownPluginList::sortByCategory);
        sortTypeMenu.addItem (203, "List Plug-ins by Manufacturer",       true, pluginSortMethod == KnownPluginList::sortByManufacturer);
        sortTypeMenu.addItem (204, "List Plug-ins Based on the Directory Structure", true, pluginSortMethod == KnownPluginList::sortByFileSystemLocation);
        menu.addSubMenu ("Plug-in Menu Type", sortTypeMenu);

        menu.addSeparator();
        menu.addCommandItem (&getCommandManager(), CommandIDs::showAudioSettings);
        menu.addCommandItem (&getCommandManager(), CommandIDs::toggleDoublePrecision);

        if (autoScaleOptionAvailable)
            menu.addCommandItem (&getCommandManager(), CommandIDs::autoScalePluginWindows);

        menu.addSeparator();
        menu.addCommandItem (&getCommandManager(), CommandIDs::aboutBox);
    }
    else if (topLevelMenuIndex == 3)
    {
        menu.addCommandItem (&getCommandManager(), CommandIDs::allWindowsForward);
    }

    return menu;
}

void MainHostWindow::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
{
    if (menuItemID == 250)
    {
        if (graphHolder != nullptr)
            if (auto* graph = graphHolder->graph.get())
                graph->clear();
    }
   #if ! (JUCE_ANDROID || JUCE_IOS)
    else if (menuItemID >= 100 && menuItemID < 200)
    {
        RecentlyOpenedFilesList recentFiles;
        recentFiles.restoreFromString (getAppProperties().getUserSettings()
                                            ->getValue ("recentFilterGraphFiles"));

        if (graphHolder != nullptr)
        {
            if (auto* graph = graphHolder->graph.get())
            {
                SafePointer<MainHostWindow> parent { this };
                graph->saveIfNeededAndUserAgreesAsync ([parent, recentFiles, menuItemID] (FileBasedDocument::SaveResult r)
                {
                    if (parent == nullptr)
                        return;

                    if (r == FileBasedDocument::savedOk)
                        parent->graphHolder->graph->loadFrom (recentFiles.getFile (menuItemID - 100), true);
                });
            }
        }
    }
   #endif
    else if (menuItemID >= 200 && menuItemID < 210)
    {
             if (menuItemID == 200)     pluginSortMethod = KnownPluginList::defaultOrder;
        else if (menuItemID == 201)     pluginSortMethod = KnownPluginList::sortAlphabetically;
        else if (menuItemID == 202)     pluginSortMethod = KnownPluginList::sortByCategory;
        else if (menuItemID == 203)     pluginSortMethod = KnownPluginList::sortByManufacturer;
        else if (menuItemID == 204)     pluginSortMethod = KnownPluginList::sortByFileSystemLocation;

        getAppProperties().getUserSettings()->setValue ("pluginSortMethod", (int) pluginSortMethod);

        menuItemsChanged();
    }
    else
    {
        if (KnownPluginList::getIndexChosenByMenu (pluginDescriptions, menuItemID) >= 0)
            createPlugin (getChosenType (menuItemID), { proportionOfWidth  (0.3f + Random::getSystemRandom().nextFloat() * 0.6f),
                                                        proportionOfHeight (0.3f + Random::getSystemRandom().nextFloat() * 0.6f) });
    }
}

void MainHostWindow::menuBarActivated (bool isActivated)
{
    if (isActivated && graphHolder != nullptr)
        graphHolder->unfocusKeyboardComponent();
}

void MainHostWindow::createPlugin (const PluginDescription& desc, Point<int> pos)
{
    if (graphHolder != nullptr)
        graphHolder->createNewPlugin (desc, pos);
}

void MainHostWindow::addPluginsToMenu (PopupMenu& m)
{
    if (graphHolder != nullptr)
    {
        int i = 0;

        for (auto& t : internalTypes)
            m.addItem (++i, t.name + " (" + t.pluginFormatName + ")");
    }

    m.addSeparator();

    pluginDescriptions = knownPluginList.getTypes();

    // This avoids showing the internal types again later on in the list
    pluginDescriptions.removeIf ([] (PluginDescription& desc)
    {
        return desc.pluginFormatName == InternalPluginFormat::getIdentifier();
    });

    KnownPluginList::addToMenu (m, pluginDescriptions, pluginSortMethod);
}

PluginDescription MainHostWindow::getChosenType (const int menuID) const
{
    if (menuID >= 1 && menuID < (int) (1 + internalTypes.size()))
        return internalTypes[(size_t) (menuID - 1)];

    return pluginDescriptions[KnownPluginList::getIndexChosenByMenu (pluginDescriptions, menuID)];
}

//==============================================================================
ApplicationCommandTarget* MainHostWindow::getNextCommandTarget()
{
    return findFirstTargetParentComponent();
}

void MainHostWindow::getAllCommands (Array<CommandID>& commands)
{
    // this returns the set of all commands that this target can perform..
    const CommandID ids[] = {
                             #if ! (JUCE_IOS || JUCE_ANDROID)
                              CommandIDs::newFile,
                              CommandIDs::open,
                              CommandIDs::save,
                              CommandIDs::saveAs,
                             #endif
                              CommandIDs::showPluginListEditor,
                              CommandIDs::showAudioSettings,
                              CommandIDs::toggleDoublePrecision,
                              CommandIDs::aboutBox,
                              CommandIDs::allWindowsForward,
                              CommandIDs::autoScalePluginWindows
                            };

    commands.addArray (ids, numElementsInArray (ids));
}

void MainHostWindow::getCommandInfo (const CommandID commandID, ApplicationCommandInfo& result)
{
    const String category ("General");

    switch (commandID)
    {
   #if ! (JUCE_IOS || JUCE_ANDROID)
    case CommandIDs::newFile:
        result.setInfo ("New", "Creates a new filter graph file", category, 0);
        result.defaultKeypresses.add(KeyPress('n', ModifierKeys::commandModifier, 0));
        break;

    case CommandIDs::open:
        result.setInfo ("Open...", "Opens a filter graph file", category, 0);
        result.defaultKeypresses.add (KeyPress ('o', ModifierKeys::commandModifier, 0));
        break;

    case CommandIDs::save:
        result.setInfo ("Save", "Saves the current graph to a file", category, 0);
        result.defaultKeypresses.add (KeyPress ('s', ModifierKeys::commandModifier, 0));
        break;

    case CommandIDs::saveAs:
        result.setInfo ("Save As...",
                        "Saves a copy of the current graph to a file",
                        category, 0);
        result.defaultKeypresses.add (KeyPress ('s', ModifierKeys::shiftModifier | ModifierKeys::commandModifier, 0));
        break;
   #endif

    case CommandIDs::showPluginListEditor:
        result.setInfo ("Edit the List of Available Plug-ins...", {}, category, 0);
        result.addDefaultKeypress ('p', ModifierKeys::commandModifier);
        break;

    case CommandIDs::showAudioSettings:
        result.setInfo ("Change the Audio Device Settings", {}, category, 0);
        result.addDefaultKeypress ('a', ModifierKeys::commandModifier);
        break;

    case CommandIDs::toggleDoublePrecision:
        updatePrecisionMenuItem (result);
        break;

    case CommandIDs::aboutBox:
        result.setInfo ("About...", {}, category, 0);
        break;

    case CommandIDs::allWindowsForward:
        result.setInfo ("All Windows Forward", "Bring all plug-in windows forward", category, 0);
        result.addDefaultKeypress ('w', ModifierKeys::commandModifier);
        break;

    case CommandIDs::autoScalePluginWindows:
        updateAutoScaleMenuItem (result);
        break;

    default:
        break;
    }
}

bool MainHostWindow::perform (const InvocationInfo& info)
{
    switch (info.commandID)
    {
   #if ! (JUCE_IOS || JUCE_ANDROID)
    case CommandIDs::newFile:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
        {
            SafePointer<MainHostWindow> parent { this };
            graphHolder->graph->saveIfNeededAndUserAgreesAsync ([parent] (FileBasedDocument::SaveResult r)
            {
                if (parent == nullptr)
                    return;

                if (r == FileBasedDocument::savedOk)
                    parent->graphHolder->graph->newDocument();
            });
        }
        break;

    case CommandIDs::open:
         if (graphHolder != nullptr && graphHolder->graph != nullptr)
         {
             SafePointer<MainHostWindow> parent { this };
             graphHolder->graph->saveIfNeededAndUserAgreesAsync ([parent] (FileBasedDocument::SaveResult r)
             {
                 if (parent == nullptr)
                     return;

                 if (r == FileBasedDocument::savedOk)
                     parent->graphHolder->graph->loadFromUserSpecifiedFileAsync (true, [] (Result) {});
             });
         }
        break;

    case CommandIDs::save:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
            graphHolder->graph->saveAsync (true, true, nullptr);
        break;

    case CommandIDs::saveAs:
        if (graphHolder != nullptr && graphHolder->graph != nullptr)
            graphHolder->graph->saveAsAsync ({}, true, true, true, nullptr);
        break;
   #endif

    case CommandIDs::showPluginListEditor:
        if (pluginListWindow == nullptr)
            pluginListWindow.reset (new PluginListWindow (*this, formatManager));

        pluginListWindow->toFront (true);
        break;

    case CommandIDs::showAudioSettings:
        showAudioSettings();
        break;

    case CommandIDs::toggleDoublePrecision:
        if (auto* props = getAppProperties().getUserSettings())
        {
            auto newIsDoublePrecision = ! isDoublePrecisionProcessingEnabled();
            props->setValue ("doublePrecisionProcessing", var (newIsDoublePrecision));

            ApplicationCommandInfo cmdInfo (info.commandID);
            updatePrecisionMenuItem (cmdInfo);
            menuItemsChanged();

            if (graphHolder != nullptr)
                graphHolder->setDoublePrecision (newIsDoublePrecision);
        }
        break;

    case CommandIDs::autoScalePluginWindows:
        if (auto* props = getAppProperties().getUserSettings())
        {
            auto newAutoScale = ! isAutoScalePluginWindowsEnabled();
            props->setValue ("autoScalePluginWindows", var (newAutoScale));

            ApplicationCommandInfo cmdInfo (info.commandID);
            updateAutoScaleMenuItem (cmdInfo);
            menuItemsChanged();
        }
        break;

    case CommandIDs::aboutBox:
        // TODO
        break;

    case CommandIDs::allWindowsForward:
    {
        auto& desktop = Desktop::getInstance();

        for (int i = 0; i < desktop.getNumComponents(); ++i)
            desktop.getComponent (i)->toBehind (this);

        break;
    }

    default:
        return false;
    }

    return true;
}

void MainHostWindow::showAudioSettings()
{
    auto* audioSettingsComp = new AudioDeviceSelectorComponent (deviceManager,
                                                                0, 256,
                                                                0, 256,
                                                                true, true,
                                                                true, false);

    audioSettingsComp->setSize (500, 450);

    DialogWindow::LaunchOptions o;
    o.content.setOwned (audioSettingsComp);
    o.dialogTitle                   = "Audio Settings";
    o.componentToCentreAround       = this;
    o.dialogBackgroundColour        = getLookAndFeel().findColour (ResizableWindow::backgroundColourId);
    o.escapeKeyTriggersCloseButton  = true;
    o.useNativeTitleBar             = false;
    o.resizable                     = false;

     auto* w = o.create();
     auto safeThis = SafePointer<MainHostWindow> (this);

     w->enterModalState (true,
                         ModalCallbackFunction::create
                         ([safeThis] (int)
                         {
                             auto audioState = safeThis->deviceManager.createStateXml();

                             getAppProperties().getUserSettings()->setValue ("audioDeviceState", audioState.get());
                             getAppProperties().getUserSettings()->saveIfNeeded();

                             if (safeThis->graphHolder != nullptr)
                                 if (safeThis->graphHolder->graph != nullptr)
                                     safeThis->graphHolder->graph->graph.removeIllegalConnections();
                         }), true);
}

bool MainHostWindow::isInterestedInFileDrag (const StringArray&)
{
    return true;
}

void MainHostWindow::fileDragEnter (const StringArray&, int, int)
{
}

void MainHostWindow::fileDragMove (const StringArray&, int, int)
{
}

void MainHostWindow::fileDragExit (const StringArray&)
{
}

void MainHostWindow::filesDropped (const StringArray& files, int x, int y)
{
    if (graphHolder != nullptr)
    {
       #if ! (JUCE_ANDROID || JUCE_IOS)
        File firstFile { files[0] };

        if (files.size() == 1 && firstFile.hasFileExtension (PluginGraph::getFilenameSuffix()))
        {
            if (auto* g = graphHolder->graph.get())
            {
                SafePointer<MainHostWindow> parent;
                g->saveIfNeededAndUserAgreesAsync ([parent, g, firstFile] (FileBasedDocument::SaveResult r)
                {
                    if (parent == nullptr)
                        return;

                    if (r == FileBasedDocument::savedOk)
                        g->loadFrom (firstFile, true);
                });
            }
        }
        else
       #endif
        {
            OwnedArray<PluginDescription> typesFound;
            knownPluginList.scanAndAddDragAndDroppedFiles (formatManager, files, typesFound);

            auto pos = graphHolder->getLocalPoint (this, Point<int> (x, y));

            for (int i = 0; i < jmin (5, typesFound.size()); ++i)
                if (auto* desc = typesFound.getUnchecked(i))
                    createPlugin (*desc, pos);
        }
    }
}

bool MainHostWindow::isDoublePrecisionProcessingEnabled()
{
    if (auto* props = getAppProperties().getUserSettings())
        return props->getBoolValue ("doublePrecisionProcessing", false);

    return false;
}

bool MainHostWindow::isAutoScalePluginWindowsEnabled()
{
    if (auto* props = getAppProperties().getUserSettings())
        return props->getBoolValue ("autoScalePluginWindows", false);

    return false;
}

void MainHostWindow::updatePrecisionMenuItem (ApplicationCommandInfo& info)
{
    info.setInfo ("Double Floating-Point Precision Rendering", {}, "General", 0);
    info.setTicked (isDoublePrecisionProcessingEnabled());
}

void MainHostWindow::updateAutoScaleMenuItem (ApplicationCommandInfo& info)
{
    info.setInfo ("Auto-Scale Plug-in Windows", {}, "General", 0);
    info.setTicked (isAutoScalePluginWindowsEnabled());
}
