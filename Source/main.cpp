#include <3ds.h>
#include "csvc.h"
#include <CTRPluginFramework.hpp>
#include "VoiceChat.hpp"
#include "Helpers.hpp"
#include <string>
#include <sstream>

namespace CTRPluginFramework
{
    /* ---------------------------------------------------------------
     * Touchscreen force-on patch (from original template - keeps CTRPF
     * usable when a game disables the touch panel during NFC scans).
     * --------------------------------------------------------------- */
    static void ToggleTouchscreenForceOn(void)
    {
        static u32 original    = 0;
        static u32 *patchAddr  = nullptr;

        if (patchAddr && original) { *patchAddr = original; return; }

        static const std::vector<u32> pattern = {
            0xE59F10C0, 0xE5840004, 0xE5841000, 0xE5DD0000,
            0xE5C40008, 0xE28DD03C, 0xE8BD80F0, 0xE5D51001,
            0xE1D400D4, 0xE3510003, 0x159F0034, 0x1A000003
        };

        Result res; Handle ph; s64 textSize = 0; s64 startAddr = 0; u32 *found;
        if (R_FAILED(svcOpenProcess(&ph, 16))) return;
        svcGetProcessInfo(&textSize,  ph, 0x10002);
        svcGetProcessInfo(&startAddr, ph, 0x10005);
        if (R_FAILED(svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x14000000, ph,
                                           (u32)startAddr, textSize)))
            goto exit;

        found = (u32 *)Utils::Search<u32>(0x14000000, (u32)textSize, pattern);
        if (found) {
            original   = found[13];
            patchAddr  = (u32 *)PA_FROM_VA(found + 13);
            found[13]  = 0xE1A00000;
        }
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x14000000, textSize);
    exit:
        svcCloseHandle(ph);
    }

    /* ---------------------------------------------------------------
     * Plugin-global VoiceChat instance + room entry pointers
     * --------------------------------------------------------------- */
    static VoicePlugin::VoiceChat *g_vc = nullptr;

    /* Pointers to the room MenuEntries so we can update their names
     * after receiving a room list from the server.               */
    static MenuEntry *g_roomEntries[MAX_ROOMS] = {};
    static int        g_roomEntryCount         = 0;

    /* ---------------------------------------------------------------
     * Helpers
     * --------------------------------------------------------------- */
    static std::string RoomLabel(int idx) {
        /* Before we have a live list just show "Room N (–/5)" */
        if (!g_vc) return "Room " + std::to_string(idx);
        const auto &rooms = g_vc->GetRooms();
        if (idx < (int)rooms.size()) {
            const auto &r = rooms[idx];
            return r.name + " (" + std::to_string(r.count) + "/" +
                   std::to_string((int)r.max) + ")";
        }
        return "Room " + std::to_string(idx) + " (0/5)";
    }

    /* Rebuild room folder entry names after a refresh */
    static void RefreshRoomNames() {
        for (int i = 0; i < g_roomEntryCount; i++) {
            if (g_roomEntries[i])
                g_roomEntries[i]->Name() = RoomLabel(i);
        }
    }

    /* ---------------------------------------------------------------
     * Menu callbacks
     * --------------------------------------------------------------- */

    /* --- Connect / Disconnect toggle -------------------------------- */
    static void CB_Connect(MenuEntry *entry)
    {
        if (!g_vc) return;

        if (entry->WasJustActivated()) {
            /* Ask for username if not yet set */
            if (g_vc->GetUsername().empty()) {
                std::string name;
                Keyboard kbd("Set username (max 15 chars):");
                kbd.SetMaxLength(15);
                if (kbd.Open(name) != 0 || name.empty()) {
                    entry->Disable();
                    return;
                }
                g_vc->SetUsername(name);
            }

            if (!g_vc->Connect()) {
                MessageBox("Voice Chat", "Failed to connect!\n\nCheck server address in Settings.")();
                entry->Disable();
            }
        } else {
            g_vc->Disconnect();
            /* Reset room entry labels */
            for (int i = 0; i < g_roomEntryCount; i++) {
                if (g_roomEntries[i])
                    g_roomEntries[i]->Name() = "Room " + std::to_string(i) + " (0/5)";
            }
        }
    }

    /* --- Mute toggle ------------------------------------------------ */
    static void CB_Mute(MenuEntry *entry)
    {
        if (!g_vc) return;
        g_vc->SetMuted(entry->IsActivated());
    }

    /* --- Voice Status overlay toggle -------------------------------- */
    /*
     * New 3DS note: the overlay draws into the game's framebuffer every
     * frame via the OSD callback. On New 3DS running at 804 MHz this is
     * fine, but on an Old 3DS with a demanding game it may cause minor
     * slowdowns. Toggle this off if you notice frame drops.
     */
    static void CB_VoiceStatus(MenuEntry *entry)
    {
        if (!g_vc) return;
        g_vc->SetOverlayEnabled(entry->IsActivated());
    }

    /* --- Refresh room list ------------------------------------------ */
    static void CB_RefreshRooms(MenuEntry *entry)
    {
        if (!g_vc) return;
        if (!g_vc->IsConnected()) {
            MessageBox("Voice Chat", "Not connected to server.\nConnect first.")();
            return;
        }
        g_vc->RequestRoomList();

        /* Give server a moment to respond (TCP is non-blocking on our side;
         * the voice thread will pick up the response within 20 ms).
         * We wait up to 500 ms polling for new data.                   */
        for (int i = 0; i < 25; i++) {
            svcSleepThread(20000000LL); /* 20 ms */
            if (!g_vc->GetRooms().empty()) break;
        }
        RefreshRoomNames();
    }

    /* --- Join room (lambda per slot, room ID captured) -------------- */
    static MenuEntry *MakeRoomEntry(int roomId)
    {
        return new MenuEntry(
            "Room " + std::to_string(roomId) + " (0/5)",
            nullptr,
            [roomId](MenuEntry *) {
                if (!g_vc) return;
                if (!g_vc->IsConnected()) {
                    MessageBox("Voice Chat", "Not connected.\nUse 'Connect to VC' first.")();
                    return;
                }
                g_vc->JoinRoom((u8)roomId);

                /* Short wait for JOIN_ROOM_ACK so the OSD updates */
                svcSleepThread(100000000LL); /* 100 ms */

                std::string info = "Joined Room ";
                info += std::to_string(roomId);
                info += "\n\nUsers in room: ";
                info += std::to_string(g_vc->GetRoomUsers().size());
                MessageBox("Voice Chat", info)();
            }
        );
    }

    /* --- Leave current room ----------------------------------------- */
    static void CB_LeaveRoom(MenuEntry *)
    {
        if (!g_vc || !g_vc->IsConnected()) return;
        g_vc->LeaveRoom();
        MessageBox("Voice Chat", "Left room.")();
    }

    /* --- Settings: set server IP ------------------------------------ */
    static void CB_SetServer(MenuEntry *)
    {
        if (!g_vc) return;
        if (g_vc->IsConnected()) {
            MessageBox("Voice Chat", "Disconnect first before\nchanging the server address.")();
            return;
        }
        std::string addr = g_vc->GetServerAddress();
        Keyboard kbd("Server IP address:");
        kbd.SetMaxLength(39); /* max IPv4 string */
        if (kbd.Open(addr) == 0 && !addr.empty())
            g_vc->SetServerAddress(addr);
    }

    /* --- Settings: set username ------------------------------------- */
    static void CB_SetUsername(MenuEntry *)
    {
        if (!g_vc) return;
        std::string name = g_vc->GetUsername();
        Keyboard kbd("Username (max 15 chars):");
        kbd.SetMaxLength(15);
        if (kbd.Open(name) == 0 && !name.empty())
            g_vc->SetUsername(name);
    }

    /* --- Settings: VAD threshold ------------------------------------ */
    static void CB_SetVAD(MenuEntry *)
    {
        /* Simple numeric keyboard for the VAD RMS threshold */
        /* (lower = more sensitive mic, higher = less sensitive) */
        MessageBox("VAD Threshold",
            "Current: 600 (default)\n\n"
            "Rebuild with AUDIO_VAD_THRESHOLD\n"
            "in Includes/Audio.hpp to change.\n\n"
            "600  = normal office noise\n"
            "1200 = louder room recommended")();
    }

    /* --- Status info popup ------------------------------------------ */
    static void CB_Status(MenuEntry *)
    {
        if (!g_vc) { MessageBox("VC Status", "Not initialized.")(); return; }

        std::string s;
        s  = "Connected : ";     s += g_vc->IsConnected()  ? "Yes" : "No";  s += "\n";
        s += "In Room   : ";
        if (g_vc->IsInRoom()) {
            s += std::to_string(g_vc->GetCurrentRoom());
        } else {
            s += "–";
        }
        s += "\n";
        s += "Muted     : ";     s += g_vc->IsMuted()       ? "Yes" : "No";  s += "\n";
        s += "Talking   : ";     s += g_vc->IsTalking()     ? "Yes" : "No";  s += "\n";
        s += "Overlay   : ";     s += g_vc->IsOverlayEnabled() ? "On" : "Off"; s += "\n";
        s += "Server    : ";     s += g_vc->GetServerAddress(); s += "\n";
        s += "Username  : ";
        s += g_vc->GetUsername().empty() ? "(not set)" : g_vc->GetUsername();

        MessageBox("VC Status", s)();
    }

    /* ---------------------------------------------------------------
     * InitMenu
     * --------------------------------------------------------------- */
    void InitMenu(PluginMenu &menu)
    {
        g_vc = new VoicePlugin::VoiceChat();

        /* ---- Top-level controls ---- */
        menu += new MenuEntry("Connect to VC",         nullptr, CB_Connect);
        menu += new MenuEntry("Mute",                  nullptr, CB_Mute);

        /*
         * Voice Status Overlay
         * Draws a small user list on the top screen.
         * Each talking user's name turns green (Discord-style).
         * DISABLE this if your game drops frames with the OSD active.
         */
        menu += new MenuEntry("Voice Status Overlay",  nullptr, CB_VoiceStatus);
        menu += new MenuEntry("Leave Room",             nullptr, CB_LeaveRoom);
        menu += new MenuEntry("Status",                 nullptr, CB_Status);

        /* ---- VC Rooms folder ---- */
        MenuFolder *rooms = new MenuFolder(
            "VC Rooms",
            "Select a room to join.\nUse 'Refresh Rooms' to update counts.");

        for (int i = 0; i < MAX_ROOMS; i++) {
            g_roomEntries[i] = MakeRoomEntry(i);
            *rooms += g_roomEntries[i];
        }
        g_roomEntryCount = MAX_ROOMS;

        *rooms += new MenuEntry("Refresh Rooms", nullptr, CB_RefreshRooms);
        menu += rooms;

        /* ---- Settings folder ---- */
        MenuFolder *settings = new MenuFolder("Settings");
        *settings += new MenuEntry("Set Server Address", nullptr, CB_SetServer);
        *settings += new MenuEntry("Set Username",        nullptr, CB_SetUsername);
        *settings += new MenuEntry("VAD Info",            nullptr, CB_SetVAD);
        menu += settings;
    }

    /* ---------------------------------------------------------------
     * Framework callbacks
     * --------------------------------------------------------------- */
    void PatchProcess(FwkSettings &settings)
    {
        ToggleTouchscreenForceOn();
        settings.TryLoadSDSounds = false;
    }

    void OnProcessExit(void)
    {
        ToggleTouchscreenForceOn();
        if (g_vc) {
            g_vc->Disconnect();
            delete g_vc;
            g_vc = nullptr;
        }
    }

    /* ---------------------------------------------------------------
     * Plugin entry point
     * --------------------------------------------------------------- */
    int main(void)
    {
        PluginMenu *menu = new PluginMenu(
            "Voice Chat", 1, 0, 0,
            "3DS Voice Chat via Opus\n"
            "Connect to VC  → connects to server\n"
            "VC Rooms       → join a voice room\n"
            "Mute           → silence your mic\n"
            "Voice Status   → on-screen user list"
        );

        menu->SynchronizeWithFrame(true);
        InitMenu(*menu);
        menu->Run();

        delete menu;
        return 0;
    }

} // namespace CTRPluginFramework
