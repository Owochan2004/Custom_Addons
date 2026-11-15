#define BOOST_ALL_NO_LIB

#include <windows.h>
#undef CreateMutex
#define CreateMutex CreateMutexW

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <set>

#include <kenshi/Character.h>
#include <kenshi/GameWorld.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Building/UseableStuff.h>
#include <kenshi/AI/AI.h>
#include <kenshi/AI/AITaskSystem.h>
#include <kenshi/Enums.h>
#include <kenshi/util/hand.h>
#include <core/Functions.h>
#include <Debug.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_LayerManager.h>
#include <mygui/MyGUI_ResourceManager.h>

#pragma comment(lib, "MyGUIEngine_x64.lib")

#undef CreateMutex
#define CreateMutex CreateMutexA

extern GameWorld* ou;

class DatapanelGUI;

namespace
{
    std::set<unsigned __int64> g_lockedBeds;
    CRITICAL_SECTION g_bedsLock;
    bool g_lockInitialized = false;
    bool g_loadedFromDisk = false;
    bool g_isRefreshing = false;
    bool g_needsPanelRefresh = false;

    Building* g_activeBed = NULL;
    MyGUI::Button* g_bedButton = NULL;

    const char* SAVE_FILE = "LockedBeds.txt";

    void LogMsg(const std::string& msg)
    {
        DebugLog(msg);
    }

    void InitLock()
    {
        if (!g_lockInitialized)
        {
            InitializeCriticalSection(&g_bedsLock);
            g_lockInitialized = true;
        }
    }

    struct ScopedLock
    {
        ScopedLock()
        {
            InitLock();
            EnterCriticalSection(&g_bedsLock);
        }
        ~ScopedLock()
        {
            LeaveCriticalSection(&g_bedsLock);
        }
    };

    unsigned __int64 MakeBedKey(const hand& h)
    {
        if (!h.isValid()) return 0;
        return ((unsigned __int64)h.serial << 32) | (unsigned __int64)h.index;
    }

    void SaveBedsToDisk()
    {
        ScopedLock lock;
        std::ofstream out(SAVE_FILE, std::ios::trunc);
        if (!out.is_open()) return;

        for (std::set<unsigned __int64>::const_iterator it = g_lockedBeds.begin();
            it != g_lockedBeds.end(); ++it)
        {
            unsigned int serial = (unsigned int)((*it) >> 32);
            unsigned int index = (unsigned int)((*it) & 0xFFFFFFFFULL);
            out << serial << ":" << index << "\n";
        }
    }

    void LoadBedsFromDiskOnce()
    {
        if (g_loadedFromDisk) return;
        g_loadedFromDisk = true;

        std::ifstream in(SAVE_FILE);
        if (!in.is_open()) return;

        ScopedLock lock;
        g_lockedBeds.clear();

        std::string line;
        while (std::getline(in, line))
        {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
            {
                unsigned int serial = (unsigned int)strtoul(line.substr(0, colon).c_str(), NULL, 10);
                unsigned int index = (unsigned int)strtoul(line.substr(colon + 1).c_str(), NULL, 10);
                unsigned __int64 key = ((unsigned __int64)serial << 32) | (unsigned __int64)index;
                if (key != 0)
                    g_lockedBeds.insert(key);
            }
        }
        LogMsg("[BedLock] Camas cargadas desde LockedBeds.txt");
    }

    bool IsValidBedBuilding(Building* b)
    {
        if (!b || !b->getGameData()) return false;
        BuildingFunction fn = b->getSpecialFunction();
        return (fn == BF_BED || fn == BF_SKELETON_BED);
    }

    bool IsBedHandleLocked(const hand& h)
    {
        unsigned __int64 key = MakeBedKey(h);
        if (key == 0) return false;

        ScopedLock lock;
        if (g_lockedBeds.empty()) return false;
        return g_lockedBeds.find(key) != g_lockedBeds.end();
    }

    void SyncBedName(Building* b)
    {
        if (!b || !IsValidBedBuilding(b)) return;

        bool locked = IsBedHandleLocked(b->getHandle());
        const std::string& curName = b->getName();
        if (locked && curName != "Bed [IA: BLOQUEADA]")
            b->setName("Bed [IA: BLOQUEADA]");
        else if (!locked && curName == "Bed [IA: BLOQUEADA]")
            b->setName("Bed [IA: PERMITIDA]");
    }

    void ToggleBed(Building* b)
    {
        if (!IsValidBedBuilding(b)) return;
        unsigned __int64 key = MakeBedKey(b->getHandle());
        if (key == 0) return;

        {
            ScopedLock lock;
            std::set<unsigned __int64>::iterator it = g_lockedBeds.find(key);
            if (it != g_lockedBeds.end())
            {
                g_lockedBeds.erase(it);
                b->setName("Bed [IA: PERMITIDA]");
                LogMsg("[BedLock] Cama desbloqueada para IA.");
            }
            else
            {
                g_lockedBeds.insert(key);
                b->setName("Bed [IA: BLOQUEADA]");
                LogMsg("[BedLock] Cama bloqueada para IA.");
            }
        }
        SaveBedsToDisk();
    }

    // --- BOTÓN UI EN MYGUI ---

    void UpdateButtonCaption(bool locked)
    {
        if (!g_bedButton) return;
        if (locked)
            g_bedButton->setCaption("IA Auto: BLOQUEADA");
        else
            g_bedButton->setCaption("IA Auto: PERMITIDA");
    }

    void OnBedButtonClick(MyGUI::Widget* _sender)
    {
        if (!g_activeBed || !IsValidBedBuilding(g_activeBed))
            return;

        ToggleBed(g_activeBed);
        UpdateButtonCaption(IsBedHandleLocked(g_activeBed->getHandle()));
        g_needsPanelRefresh = true;
    }

    void UpdateBedButtonUI()
    {
        MyGUI::Gui* gui = MyGUI::Gui::getInstancePtr();
        if (!gui) return;

        if (!g_activeBed || !IsValidBedBuilding(g_activeBed))
        {
            if (g_bedButton && g_bedButton->getVisible())
                g_bedButton->setVisible(false);
            return;
        }

        if (!g_bedButton)
        {
            const char* skin = MyGUI::ResourceManager::getInstance().isExist("Kenshi_Button1")
                ? "Kenshi_Button1"
                : "Button";
            const char* layer = MyGUI::LayerManager::getInstance().isExist("Main")
                ? "Main"
                : "Overlapped";

            RECT rc;
            int viewHeight = 1080;
            HWND hwnd = GetActiveWindow();
            if (hwnd && GetClientRect(hwnd, &rc) && (rc.bottom - rc.top) > 0)
            {
                viewHeight = rc.bottom - rc.top;
            }

            int btnWidth = 220;
            int btnHeight = 30;
            int btnLeft = 12;
            int btnTop = (int)(viewHeight * 0.685f) - btnHeight - 4;
            if (btnTop < 10) btnTop = 10;

            MyGUI::Widget* w = gui->createWidgetT(
                "Button",
                skin,
                btnLeft,
                btnTop,
                btnWidth,
                btnHeight,
                MyGUI::Align::Default,
                layer,
                "BedLockToggleButton"
            );

            if (w)
            {
                g_bedButton = w->castType<MyGUI::Button>(false);
                if (g_bedButton)
                {
                    g_bedButton->setTextAlign(MyGUI::Align::Center);
                    g_bedButton->eventMouseButtonClick += MyGUI::newDelegate(OnBedButtonClick);
                }
            }
        }

        if (g_bedButton)
        {
            UpdateButtonCaption(IsBedHandleLocked(g_activeBed->getHandle()));
            if (!g_bedButton->getVisible())
                g_bedButton->setVisible(true);
        }
    }

    // --- HOOKS NATIVOS ---

    bool (*CouldIOperateOriginal)(const UseableStuff*, const hand&) = NULL;
    bool CouldIOperateHook(const UseableStuff* self, const hand& h)
    {
        bool allowed = CouldIOperateOriginal ? CouldIOperateOriginal(self, h) : true;
        if (!allowed || !self) return allowed;

        {
            ScopedLock lock;
            if (g_lockedBeds.empty()) return true;
        }

        if (IsBedHandleLocked(self->getHandle()))
        {
            // Permitir si el jugador dio orden manual; ignorar si es IA automática
            if (h.isValid())
            {
                Character* c = h.getCharacter();
                if (c)
                {
                    AI* ai = c->getAI();
                    AITaskSytem* ts = ai ? ai->getTaskSystem() : NULL;
                    if (ts && ts->hasPlayerOrders())
                    {
                        return true;
                    }
                }
            }
            return false;
        }
        return true;
    }

    void (*BuildingGetGUIDataOriginal)(Building*, DatapanelGUI*, int) = NULL;
    void BuildingGetGUIDataHook(Building* self, DatapanelGUI* datapanel, int category)
    {
        LoadBedsFromDiskOnce();

        if (self && IsValidBedBuilding(self))
        {
            SyncBedName(self);
            g_activeBed = self;
        }

        if (BuildingGetGUIDataOriginal)
            BuildingGetGUIDataOriginal(self, datapanel, category);
    }

    void (*UseableStuffGetGUIDataOriginal)(UseableStuff*, DatapanelGUI*, int) = NULL;
    void UseableStuffGetGUIDataHook(UseableStuff* self, DatapanelGUI* datapanel, int category)
    {
        LoadBedsFromDiskOnce();

        if (self && IsValidBedBuilding(self))
        {
            SyncBedName(self);
            g_activeBed = self;
        }

        if (UseableStuffGetGUIDataOriginal)
            UseableStuffGetGUIDataOriginal(self, datapanel, category);
    }

    void (*PlayerInterfaceUpdateOriginal)(PlayerInterface*) = NULL;
    void (*PlayerInterfaceSelectObjectOriginal)(PlayerInterface*, RootObject*, bool) = NULL;

    void RefreshActiveBedPanel(PlayerInterface* pi)
    {
        if (!pi || !g_activeBed || !IsValidBedBuilding(g_activeBed) || !PlayerInterfaceSelectObjectOriginal)
            return;

        Building* bedToKeep = g_activeBed;
        g_isRefreshing = true;
        pi->selectedObject = hand();
        PlayerInterfaceSelectObjectOriginal(pi, bedToKeep, false);
        g_isRefreshing = false;
    }

    void PlayerInterfaceSelectObjectHook(
        PlayerInterface* self,
        RootObject* object,
        bool modifier)
    {
        if (!g_isRefreshing)
        {
            g_activeBed = NULL;
        }

        if (PlayerInterfaceSelectObjectOriginal)
            PlayerInterfaceSelectObjectOriginal(self, object, modifier);
    }

    void PlayerInterfaceUpdateHook(PlayerInterface* self)
    {
        if (PlayerInterfaceUpdateOriginal)
            PlayerInterfaceUpdateOriginal(self);

        if (g_activeBed && self && self->selectedObject.getBuilding() != g_activeBed)
        {
            g_activeBed = NULL;
        }

        if (g_needsPanelRefresh)
        {
            g_needsPanelRefresh = false;
            RefreshActiveBedPanel(self);
        }

        UpdateBedButtonUI();
    }
}

__declspec(dllexport) void startPlugin()
{
    InitLock();
    LogMsg("[BedLock] Plugin inicializado correctamente.");

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&PlayerInterface::update),
        &PlayerInterfaceUpdateHook,
        &PlayerInterfaceUpdateOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&PlayerInterface::selectObject),
        &PlayerInterfaceSelectObjectHook,
        &PlayerInterfaceSelectObjectOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&UseableStuff::_NV_couldIOperate),
        &CouldIOperateHook,
        &CouldIOperateOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Building::_NV_getGUIData),
        &BuildingGetGUIDataHook,
        &BuildingGetGUIDataOriginal);

    KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&UseableStuff::_NV_getGUIData),
        &UseableStuffGetGUIDataHook,
        &UseableStuffGetGUIDataOriginal);
}