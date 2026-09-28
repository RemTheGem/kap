#include <windows.h>
#include <UIAutomation.h>
#include <atlbase.h>
#include <atlcomcli.h>
#include <iostream>
#include <array>
#include <vector>
#include <string>
#include <cwctype>
#include <fcntl.h>
#include <io.h>


using namespace ATL;


namespace
{
  // IDs
  constexpr int hotKeyId = 1;
  constexpr wchar_t windowClassName[] = L"HintOverlayWindowClass";
  constexpr UINT_PTR peekTimerId = 1;
  constexpr UINT_PTR refreshTimerId = 2;
  constexpr UINT_PTR manualRefreshTimerId = 3;
  constexpr UINT_PTR cursorMovementTimerId = 4;

  // hint alphabets
  const std::wstring alphabets = L"asdfghjklqwertyuiopzxcvbnm";

  // hint colors
  constexpr COLORREF transparentKey = RGB(1,1,1);
  constexpr COLORREF hintBg = RGB(255, 0, 0);
  constexpr COLORREF hintBgPrefixMatch = RGB(120, 220, 120);
  constexpr COLORREF hintText = RGB(20, 20, 20);

  // UI automation Id and its label for console outputs
  struct PatternInfo{
    PROPERTYID propertyId;
    const wchar_t* label;
  };

  // UI automation patterns that we are interested in (buttons and shit)
  constexpr std::array<PatternInfo, 4> interestingPatterns{{
    {UIA_IsInvokePatternAvailablePropertyId, L"Invoke"},
    {UIA_IsTogglePatternAvailablePropertyId, L"Toggle"},
    {UIA_IsSelectionItemPatternAvailablePropertyId, L"SelectionItem"},
    {UIA_IsExpandCollapsePatternAvailablePropertyId, L"ExpandCollapse"},
  }};

  // clickable elements like buttons and toggles
  struct ClickableElement{
    std::wstring name;
    std::wstring hint;
    RECT rect{};
    POINT center{};
  };

  // global variables

  CComPtr<IUIAutomation> automation;
  std::vector<ClickableElement> elements;
  std::wstring inputBuffer;
  HWND originalForeground = nullptr;
  HWND overlayHwnd = nullptr;
  HHOOK keyboardHook = nullptr;
  bool hintModeActive = false;
  int overlayOriginX = 0;
  int overlayOriginY = 0;
  HFONT hintFontLarge = nullptr;
  HFONT hintFontMedium = nullptr;
  HFONT hintFontSmall = nullptr;
  bool draggingCursorHeld = false;

  bool leftHeld = false;
  bool rightHeld = false;
  bool upHeld = false;
  bool downHeld = false;

  // Check if UI element has any of the patterns we are interested in
  bool HasInterestingPattern(IUIAutomationElement *element){
    for(const auto &pattern : interestingPatterns){
      CComVariant value;
      if(SUCCEEDED(element->GetCurrentPropertyValue(pattern.propertyId, &value))){
        if(value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE) return true;
      }
    }
    return false;
  }

  // UI Automation condition to check if any of the VISIBLE elements have at least one
  // of our interesting patterns
  CComPtr<IUIAutomationCondition> BuildInterestingElementsCondition(IUIAutomation *automation){
    CComPtr<IUIAutomationCondition> patternCondition;
    for(const auto &pattern : interestingPatterns){
      CComVariant trueVal(true);
      CComPtr<IUIAutomationCondition> condition;
      automation->CreatePropertyCondition(pattern.propertyId, trueVal, &condition);

      if(!patternCondition){
        patternCondition = condition;
      }
      else{
        CComPtr<IUIAutomationCondition> merged;
        // Combine patterns with OR. each element needs only one condition
        automation->CreateOrCondition(patternCondition, condition, &merged);
        patternCondition = merged;
      }
    }

    CComVariant onscreenVal(false);
    CComPtr<IUIAutomationCondition> onscreenCondition;
    automation->CreatePropertyCondition(UIA_IsOffscreenPropertyId, onscreenVal, &onscreenCondition);

    CComPtr<IUIAutomationCondition> combined;
    automation->CreateAndCondition(patternCondition, onscreenCondition, &combined);
    return combined;
  }

  // Generate hints of size count (number of interesting elements)
  // Keep increasing hint size if we run out of alphabets ex. 27 elements > 26 alphabets so use double letters
  std::vector<std::wstring> GenerateHints(size_t count){
    std::vector<std::wstring> hints;
    if(count == 0) return hints;

    // number of characters in hints
    size_t length = 1;
    // number of hints possible with current length
    size_t capacity = alphabets.size();
    // increase the number of characters in hints if the current amount doesnt fit all hints
    while(capacity < count){
      length++;
      capacity *= alphabets.size();
    }

    // store the current position of each character of the hint
    std::vector<size_t> indices(length, 0);
    for(size_t n = 0; n < count; n++){
      std::wstring hint(length, L' ');
      for(size_t i = 0; i < length; i++) hint[i] = alphabets[indices[i]];
      hints.push_back(hint);
      // increment the hint like odometer. carry to the left when out of characters for this index
      for(size_t i = length; i-- > 0;){
        if(++indices[i] < alphabets.size()) break;
        indices[i] = 0;
      }
    }
    return hints;
  }

  // Find visible clickable UI elements in current window, filter for interesting patterns
  // and store the eligible elements' name, bounds and coordinates for later
  std::vector<ClickableElement> GetClickableElements(IUIAutomation *automation, HWND window){
    std::vector<ClickableElement> results;
    if(!window) return results;

    CComPtr<IUIAutomationElement> root;
    if(FAILED(automation->ElementFromHandle(window, &root)) || !root) return results;

    CComPtr<IUIAutomationCondition> condition = BuildInterestingElementsCondition(automation);
    CComPtr<IUIAutomationElementArray> elements;
    if(FAILED(root->FindAll(TreeScope_Descendants, condition, &elements)) || ! elements) return results;

    int count = 0;
    elements->get_Length(&count);

    for(int i = 0; i < count; i++){
      // mElement is MyElement. tryna avoid shadow variables cuz i didnt pick good names T-T
      CComPtr<IUIAutomationElement> mElement;
      elements->GetElement(i, &mElement);
      if (!mElement)
        continue;
      if (!HasInterestingPattern(mElement))
        continue;

      RECT rect{};
      mElement->get_CurrentBoundingRectangle(&rect);
      // ignore invalid or empty rects
      if(rect.right <= rect.left || rect.bottom <= rect.top) continue;

      CComBSTR name;
      mElement->get_CurrentName(&name);

      // set values for element
      ClickableElement entry;
      // copy element's name. if none then unnnamed
      entry.name = name.Length() ? std::wstring(name.m_str, name.Length()) : L"(unnamed)";
      entry.rect = rect;
      entry.center.x = (rect.left + rect.right) / 2;
      entry.center.y = (rect.top + rect.bottom) / 2;
      results.push_back(std::move(entry));
    }
    return results;
  }

  // Find primary and secondary (if available) taskbar windows and return their window handle(s)
  std::vector<HWND> FindTaskbarWindows(){
    std::vector<HWND> result;
    // get main taskbar
    if(HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr)){
      result.push_back(primary);
    }
    HWND secondary = nullptr;
    // check for taskbars for other screens if any
    while((secondary = FindWindowExW(nullptr, secondary, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr){
      result.push_back(secondary);
    }
    return result;
  }

  // combine clickable elements from foreground window and taskbar windows
  std::vector<ClickableElement> GetAllClickableElements(IUIAutomation *automation, HWND foregroundWindow){
    std::vector<ClickableElement> combined = GetClickableElements(automation, foregroundWindow);

    for(HWND taskbarHwnd : FindTaskbarWindows()){
      if(taskbarHwnd == foregroundWindow) continue;
      std::vector<ClickableElement> taskbarElements = GetClickableElements(automation, taskbarHwnd);
      combined.insert(combined.end(), taskbarElements.begin(), taskbarElements.end());
    }
    return combined;
  }

  // Move cursor to x and y and simulate left click
  void ClickAt(int x, int y){
    SetCursorPos(x, y);
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
  }

  // hide Kap overlay (hints) and reset input states
  // like releasing mouse button if it was held for drag
  void HideOverlay(HWND overlayHwnd){
    if(draggingCursorHeld){
      INPUT input{};
      input.type = INPUT_MOUSE;
      input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
      SendInput(1, &input, sizeof(INPUT));
      draggingCursorHeld = false;
    }
    ShowWindow(overlayHwnd, SW_HIDE);
    hintModeActive = false;
    inputBuffer.clear(); 
  }

  // Refresh clickable elements for current foreground window
  // and (re)assign hints for each element (no visible difference in the overlay)
  void RefreshElementsForForeground(){
    originalForeground = GetForegroundWindow();
    elements = GetAllClickableElements(automation, originalForeground);

    std::vector<std::wstring> hints = GenerateHints(elements.size());
    for(size_t i = 0; i < elements.size(); i++){
      elements[i].hint = hints[i];
    }
    inputBuffer.clear();
  }

  // Click the selected UI element and schedule an overlay refresh 
  // timer to ensure everything loads. May need adjusting
  void ClickElementAndContinue(HWND overlayHwnd, const ClickableElement &element){
    std::wcout << L"Clicking: " << element.name << L"\n";
    ClickAt(element.center.x, element.center.y);
    // wait 150 ms before refreshing. wait cuz things need to load
    SetTimer(overlayHwnd, refreshTimerId, 150, nullptr);
  }

  // Find the element matching the current input and click it
  // if no match yet then wait for more input
  // else clear input buffer
  void FindHintAndClick(HWND overlayHwnd){
    for(const auto &element : elements){
      if(element.hint == inputBuffer){
        ClickElementAndContinue(overlayHwnd, element);
        return;
      }
    }

    bool anyMatches = false;
    for(const auto &element : elements){
      if(element.hint.rfind(inputBuffer, 0) == 0){
        anyMatches = true;
        break;
      }
    }
    // you wrote total bs? no problem. clear buffer
    if(!anyMatches) inputBuffer.clear();
    InvalidateRect(overlayHwnd, nullptr, FALSE);
  }

  // Start Hint Mode: Find clickable elements, assign hints
  // and display the overlay for the eligible window(s)
  void ShowOverlayForForeground(HWND overlayHwnd){
    originalForeground = GetForegroundWindow();
    if(!originalForeground) return;

    elements = GetAllClickableElements(automation, originalForeground);
    if(elements.empty()){
      std::wcout << L"No clickable elements here T-T.\n";
      return;
    }
    std::vector<std::wstring> hints = GenerateHints(elements.size());
    for(size_t i = 0; i < elements.size(); i++){
      elements[i].hint = hints[i];
    }
    inputBuffer.clear();
    overlayOriginX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    overlayOriginY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    SetWindowPos(overlayHwnd, HWND_TOPMOST, overlayOriginX, overlayOriginY, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    hintModeActive = true;
    InvalidateRect(overlayHwnd, nullptr, TRUE);
  }

  // Refresh the clickable elements and redraw the overlay
  // if no elements then hide overlay
  void RefreshAndRedraw(HWND hwnd){
    RefreshElementsForForeground();
    if(elements.empty()){
      std::wcout << L"No clickable elements found. exiting.\n";
      HideOverlay(hwnd);
    }
    else{
      SetWindowPos(hwnd, HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      InvalidateRect(hwnd, nullptr, TRUE);
    }
  }

  // Check if two element rects overlap
  bool RectsOverlap(const RECT &a, const RECT &b)
  {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
  }

  // Draw the hints and their rects while avoiding overlap between labels
  void PaintOverlay(HWND hwnd){
    PAINTSTRUCT paintStruct;
    HDC hdc = BeginPaint(hwnd, &paintStruct);

    RECT client{};
    GetClientRect(hwnd, &client);
    HBRUSH backgroundBrush = CreateSolidBrush(transparentKey);
    FillRect(hdc, &client, backgroundBrush);
    DeleteObject(backgroundBrush);
    SetBkMode(hdc, TRANSPARENT);

    std::vector<RECT> placedLabels;
    HFONT candidateFonts[] = {hintFontLarge, hintFontMedium, hintFontSmall};

    for(const auto &element : elements){
      bool isHintMatch = !inputBuffer.empty() && element.hint.rfind(inputBuffer, 0) == 0;
      constexpr int padding = 4;
      RECT labelRect{};
      HFONT chosenFont = hintFontSmall;
      // Choose the right font size based on how much space we have
      for(HFONT font : candidateFonts){
        SelectObject(hdc, font);
        RECT measure{};
        DrawTextW(hdc, element.hint.c_str(), -1, &measure, DT_CALCRECT | DT_NOPREFIX);
        RECT candidate;
        candidate.left = element.rect.left - overlayOriginX;
        candidate.top = element.rect.top - overlayOriginY;
        candidate.right = candidate.left + (measure.right - measure.left) + padding * 2;
        candidate.bottom = candidate.top + (measure.bottom - measure.top) + padding * 2;
        bool overlaps = false;
        for(const auto &placed : placedLabels){
          if(RectsOverlap(candidate, placed)){ overlaps = true; break;}
        }
        labelRect = candidate;
        chosenFont = font;
        if(!overlaps) break;
      }
      placedLabels.push_back(labelRect);
      HBRUSH labelBrush = CreateSolidBrush(isHintMatch ? hintBgPrefixMatch : hintBg);
      FillRect(hdc, &labelRect, labelBrush);
      DeleteObject(labelBrush);
      SetTextColor(hdc, hintText);
      RECT textRect = labelRect;
      DrawTextW(hdc, element.hint.c_str(), -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    EndPaint(hwnd, &paintStruct);
  }


  // Global low level keyboard hook to handle hint input, overlay controls and mouse simulations
  LRESULT CALLBACK ProcessKeyboardTransparent(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode == HC_ACTION && hintModeActive){

      bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
      bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
      auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
      DWORD virtualKeycode = info->vkCode;
      // cursor movement
      if(isKeyDown || isKeyUp){
        bool *heldFlag = nullptr;
        if(virtualKeycode == VK_LEFT)
          heldFlag = &leftHeld;
        if (virtualKeycode == VK_RIGHT)
          heldFlag = &rightHeld;
        if (virtualKeycode == VK_UP)
          heldFlag = &upHeld;
        if (virtualKeycode == VK_DOWN)
          heldFlag = &downHeld;
        if(heldFlag){
          *heldFlag = isKeyDown;
          if(isKeyDown){
            SetTimer(overlayHwnd, cursorMovementTimerId, 2, nullptr);
            return 1;
          }
        }
      }
      if(isKeyDown){
        // exit hint mode
        if(virtualKeycode == VK_ESCAPE){
          HideOverlay(overlayHwnd);
          return 1;
        }
        // remove last typed character
        if(virtualKeycode == VK_BACK){
          if(!inputBuffer.empty()) inputBuffer.pop_back();
          InvalidateRect(overlayHwnd, nullptr, FALSE);
          return 1;
        }
        // peek (hide hint overlay for half a sec)
        if(virtualKeycode == VK_LSHIFT){
          ShowWindow(overlayHwnd, SW_HIDE);
          SetTimer(overlayHwnd, peekTimerId, 500, nullptr);
          return 1;
        }
        // press and hold left mouse button. again to turn off
        if(virtualKeycode == VK_SPACE){
          draggingCursorHeld = !draggingCursorHeld;
          INPUT input{};
          input.type = INPUT_MOUSE;
          input.mi.dwFlags = draggingCursorHeld ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
          SendInput(1, &input, sizeof(INPUT));
          return 1;
        }
        // Right click mouse
        if (virtualKeycode == VK_RETURN && (GetAsyncKeyState(VK_CONTROL ) & 0x8000))
        {
          INPUT input[2] = {};
          input[0].type = INPUT_MOUSE;
          input[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
          input[1].type = INPUT_MOUSE;
          input[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
          SendInput(2, input, sizeof(INPUT));
          return 1;
        }
        // left click mouse
        if(virtualKeycode == VK_RETURN){
          POINT point;
          GetCursorPos(&point);
          ClickAt(point.x, point.y);
          return 1;
        }
        // refresh overlay
        if(virtualKeycode == VK_OEM_3){
          SetTimer(overlayHwnd, manualRefreshTimerId, 1, nullptr);
          return 1;
        }
        // type shit
        if(virtualKeycode >= 'A' && virtualKeycode <= 'Z'){
          inputBuffer += static_cast<wchar_t>(towlower(static_cast<wchar_t>(virtualKeycode)));
          FindHintAndClick(overlayHwnd);
          return 1;
        }
      }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
  }

  // Handle messages sent to overlay window including hotkey, painting, timers and destruction
  LRESULT CALLBACK WindowProcess(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam){
    switch(msg){
      // toggle hint mode with hotkey
      case WM_HOTKEY:
      if(wParam == hotKeyId){
        ShowOverlayForForeground(hwnd);
      }
      return 0;

      // paint the overlay
      case WM_PAINT:
      PaintOverlay(hwnd);
      return 0;

      // DESTROY window
      case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

      // timers of various uses
      case WM_TIMER:
      if(!hintModeActive) return 0;
      if(wParam == peekTimerId){
        KillTimer(hwnd, peekTimerId);
        if(hintModeActive) ShowWindow(hwnd, SW_SHOWNA);
        }
        else if(wParam == manualRefreshTimerId || wParam == refreshTimerId){
          KillTimer(hwnd, wParam);
          RefreshAndRedraw(hwnd);
        }
        else if(wParam == cursorMovementTimerId){
          if(!leftHeld && !rightHeld && !upHeld && !downHeld) KillTimer(hwnd, cursorMovementTimerId);
          else{
            while(ShowCursor(TRUE) < 0);
            constexpr int step = 10;
            POINT point;
            GetCursorPos(&point);
            if(leftHeld)
              point.x -= step;
            if (rightHeld)
              point.x += step;
            if (upHeld)
              point.y -= step;
            if (downHeld)
              point.y += step;

            SetCursorPos(point.x, point.y);

          }
        }
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
  }

  // Register the overlay window class and create a transparent, topmost popup window to display our hints
  HWND CreateOverlay(HINSTANCE instance){

    WNDCLASSW wc{};
    wc.lpfnWndProc = WindowProcess;
    wc.hInstance = instance;
    wc.lpszClassName = windowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);


    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT, windowClassName,
      L"Hint Overlay", WS_POPUP, 0,0,0,0, nullptr, nullptr, instance, nullptr);

      if(hwnd){
        SetLayeredWindowAttributes(hwnd, transparentKey, 255, LWA_COLORKEY);
      }
      return hwnd;
  }
}

// MAIN: init COM and UI Automation, create the overlay,
// register global hotkey and keyboard hook then run message loop
// until the program exits
int wmain(){
  _setmode(_fileno(stdout), _O_U16TEXT);
  _setmode(_fileno(stdin), _O_U16TEXT);

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  if(FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))){
    std::wcerr << L"Coinitializes failed. \n";
    return 1;
  }

  HRESULT hResult = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
  if(FAILED(hResult)){
    std::wcerr << L"Failed to create UIAutomatoin instance. \n";
    CoUninitialize();
    return 1;
  }

  HINSTANCE mInstance = GetModuleHandleW(nullptr);
  HWND mOverlayHwnd = CreateOverlay(mInstance);
  if (!mOverlayHwnd)
  {
    std::wcerr << L"Failed to create overlay window. \n";
    CoUninitialize();
    return 1;
  }

  overlayHwnd = mOverlayHwnd;
  hintFontLarge = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  hintFontMedium = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  hintFontSmall = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  if(!RegisterHotKey(overlayHwnd, hotKeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_SPACE)){
    std::wcerr << L"Register hotkey failed. \n ";
    CoUninitialize();
    return 1;
  }
  keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, ProcessKeyboardTransparent, mInstance, 0);
  if(!keyboardHook){
    std::wcerr << L"SetWindowsHookExW failed. \n ";
    CoUninitialize();
    return 1;
  }

  std::wcout << "READY. Focus on a window and press CTRL+SHIFT+SPACE.\n";
  MSG msg;
  while(GetMessage(&msg, nullptr, 0, 0)){
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }

  UnhookWindowsHookEx(keyboardHook);
  UnregisterHotKey(overlayHwnd, hotKeyId);
  CoUninitialize();
  return 0;
}