#pragma once
#include <windows.h>

void InitControlPanelInstance(HINSTANCE hDll);
void ShowControlPanel(HWND parentWindow);
bool DidSettingsChange();
void ClearSettingsChangedFlag();
