#include "pch.h"

//---------------------------------------------------------------------
//		フィルタ構造体のポインタを渡す関数
//---------------------------------------------------------------------
EXTERN_C FILTER_DLL __declspec(dllexport) * __stdcall GetFilterTable(void)
{
	static TCHAR g_filterName[] = TEXT("エイリアスフォルダ選択");
	static TCHAR g_filterInformation[] = TEXT("エイリアスフォルダ選択 version 1.0.0 by 蛇色");

	static FILTER_DLL g_filter =
	{
		FILTER_FLAG_NO_CONFIG |
		FILTER_FLAG_ALWAYS_ACTIVE |
		FILTER_FLAG_DISP_FILTER |
		FILTER_FLAG_EX_INFORMATION |
		0,
		0, 0,
		g_filterName,
		NULL, NULL, NULL,
		NULL, NULL,
		NULL, NULL, NULL,
		NULL,
		func_init,
		func_exit,
		NULL,
		NULL,
		NULL, NULL,
		NULL,
		NULL,
		g_filterInformation,
		NULL, NULL,
		NULL, NULL, NULL, NULL,
		NULL,
	};

	return &g_filter;
}

//---------------------------------------------------------------------

// AviUtl (exedit) 側のコントロールの ID。
const UINT ID_ALIAS_NAME = 0x00AB;
const UINT ID_ALIAS_FOLDER = 0x00AC;

HINSTANCE g_instance = 0; // この DLL のハンドル
TCHAR g_lastAliasFolderPath[MAX_PATH] = {}; // 最後に選択したエイリアスフォルダのファイルパス

HHOOK g_hook = 0;
HWND g_targetDialog = 0;
HWND g_container = 0;
HWND g_label = 0;
HWND g_aliasFolderList = 0;

int GetWidth(LPCRECT rc);
int GetHeight(LPCRECT rc);
void SetClientRect(HWND hwnd, int x, int y, int w, int h);

int findString(HWND comboBox, LPCTSTR text);
tstring getCurrentText(HWND comboBox);
void setCurrentText(HWND comboBox, LPCTSTR text);
BOOL hasAliasFile(LPCTSTR origin, LPCTSTR fileName);
void constructAliasFolderList();

void createContainer(HWND targetDialog);
void destroyContainer();
void showContainer();
void hideContainer();

LRESULT CALLBACK container_wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

void hook();
void unhook();
LRESULT CALLBACK cwprProc(int code, WPARAM wParam, LPARAM lParam);

//---------------------------------------------------------------------

// 矩形の幅を返す
inline int GetWidth(LPCRECT rc)
{
	return rc->right - rc->left;
}

// 矩形の高さを返す
inline int GetHeight(LPCRECT rc)
{
	return rc->bottom - rc->top;
}

// クライアント領域が指定された位置に来るようにウィンドウの位置を調整する
void SetClientRect(HWND hwnd, int x, int y, int w, int h)
{
	RECT rcWindow; ::GetWindowRect(hwnd, &rcWindow);
	RECT rcClient; ::GetClientRect(hwnd, &rcClient);
//	::MapWindowPoints(hwnd, NULL, (LPPOINT)&rcClient, 2); // 幅と高さしか使わないので座標系を合わせる必要はない
	int rcWindowWidth = GetWidth(&rcWindow);
	int rcWindowHeight = GetHeight(&rcWindow);
	int rcClientWidth = GetWidth(&rcClient);
	int rcClientHeight = GetHeight(&rcClient);
	POINT diff =
	{
		rcWindowWidth - rcClientWidth,
		rcWindowHeight - rcClientHeight,
	};

	x -= diff.x / 2;
	y -= diff.y / 2;
	w += diff.x;
	h += diff.y;

	::SetWindowPos(hwnd, NULL,
		x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

//---------------------------------------------------------------------

// コンボボックスのリストから text を見つけてそのインデックスを返す
// (CB_FINDSTRING は部分一致でもヒットするので使用できない)
int findString(HWND comboBox, LPCTSTR text)
{
//	_RPTFTN(_T("findString(0x%08X, %s)\n"), comboBox, text);

	int count = ComboBox_GetCount(comboBox);
	for (int i = 0; i < count; i++)
	{
		// このアイテムのテキストの長さを取得する。
		int textLength2 = ComboBox_GetLBTextLen(comboBox, i);
		_RPTF_NUM(textLength2);
		if (textLength2 <= 0) continue;

		// このアイテムのテキストを取得する。
		tstring text2(textLength2, _T('\0'));
		ComboBox_GetLBText(comboBox, i, &text2[0]);
		_RPTF_STR(text2.c_str());
		if (text2.empty()) continue;

		if (::lstrcmp(text2.c_str(), text) == 0)
			return i; // テキストが見つかった
	}

	return CB_ERR; // テキストが見つからなかった
}

// コンボボックスの選択されているテキストを返す
tstring getCurrentText(HWND comboBox)
{
	// 選択インデックスを取得する。
	int sel = ComboBox_GetCurSel(comboBox);
	_RPTF_NUM(sel);
	if (sel < 0) return _T("");

	// テキストの長さを取得する。
	int textLength = ComboBox_GetLBTextLen(comboBox, sel);
	_RPTF_NUM(textLength);
	if (textLength <= 0) return _T("");

	// テキストを取得する。
	tstring text(textLength, _T('\0'));
	ComboBox_GetLBText(comboBox, sel, &text[0]);
	_RPTF_STR(text.c_str());

	// テキストを返す。
	return text;
}

// コンボボックスのテキストを選択する
void setCurrentText(HWND comboBox, LPCTSTR text)
{
	int index = findString(comboBox, text);
	ComboBox_SetCurSel(comboBox, index);
}

// エイリアスファイルが含まれているフォルダか判定する
// origin は親フォルダのファイルパス
// fileName はフォルダのファイルスペック
BOOL hasAliasFile(LPCTSTR origin, LPCTSTR fileName)
{
	_RPTFTN(_T("hasAliasFile(%s, %s)\n"), origin, fileName);

	// フォルダのファイルパスを取得する。
	TCHAR path[MAX_PATH] = {};
	::PathCombine(path, origin, fileName);
	::PathAppend(path, _T("*.exa"));
	_RPTF_STR(path);

	WIN32_FIND_DATA ffd = {};
	HANDLE handle = ::FindFirstFile(path, &ffd);

	if (handle == INVALID_HANDLE_VALUE)
		return FALSE; // エイリアスファイルが見つからなかった

	::FindClose(handle);

	return TRUE; // エイリアスファイルが見つかった
}

// エイリアスフォルダを列挙してコンボボックスのドロップダウンリストを構築する
void constructAliasFolderList()
{
	_RPTFT0(_T("constructAliasFolderList()\n"));

	// 起点となるフォルダのファイルパスを取得する。
	TCHAR origin[MAX_PATH] = {};
	::GetModuleFileName(NULL, origin, MAX_PATH);
	::PathRemoveFileSpec(origin);
	_RPTF_STR(origin);

	// フォルダのファイルパスを取得する。
	TCHAR path[MAX_PATH] = {};
	::PathCombine(path, origin, _T("*.*"));
	_RPTF_STR(path);

	WIN32_FIND_DATA ffd = {};
	HANDLE handle = ::FindFirstFile(path, &ffd);

	if (INVALID_HANDLE_VALUE == handle)
	{
		_RPTFT0(_T("::FindFirstFile() が失敗しました\n"));
		return;
	}

	do
	{
		if (::lstrcmp(ffd.cFileName, _T(".")) == 0 ||
			::lstrcmp(ffd.cFileName, _T("..")) == 0)
		{
			continue;
		}

		_RPTFTN(_T("+ %s をチェックします\n"), ffd.cFileName);

		if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		{
			_RPTFTN(_T("%s はフォルダではありません\n"), ffd.cFileName);
			continue;
		}

		if (!hasAliasFile(origin, ffd.cFileName))
		{
			_RPTFTN(_T("%s にエイリアスファイルは含まれていません\n"), ffd.cFileName);
			continue;
		}

		_RPTFTN(_T("%s をコンボボックスに追加します\n"), ffd.cFileName);

		ComboBox_AddString(g_aliasFolderList, ffd.cFileName);
	}
	while (::FindNextFile(handle, &ffd) != 0);

	::FindClose(handle);
}

// コンテナウィンドウとコントロール群を作成し、初期設定する
void createContainer(HWND targetDialog)
{
	_RPTFTN(_T("createContainer(0x%08X)\n"), targetDialog);

	g_targetDialog = targetDialog;
	_RPTF_HEX(g_targetDialog);

	// コントロールで使うフォントを取得する。
	// (フォントハンドルは AviUtl の API で取得することも可能)
	HFONT font = GetWindowFont(g_targetDialog);
	_RPTF_HEX(font);

	// コンテナウィンドウのウィンドウクラスを登録する。
	WNDCLASSEX wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpszClassName = _T("SelectAliasFolder");
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = container_wndProc;
	wc.hInstance = g_instance;
	wc.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW  + 1);
	::RegisterClassEx(&wc);

	// コンテナウィンドウを作成する。
	g_container = ::CreateWindowEx(
		WS_EX_NOACTIVATE,
		_T("SelectAliasFolder"),
		_T("SelectAliasFolder"),
		WS_CLIPCHILDREN | WS_CLIPSIBLINGS |
		WS_POPUP,
//		WS_POPUP | WS_BORDER, // コンテナウィンドウに細い枠を付けたいならこっちを使う
//		WS_POPUP | WS_DLGFRAME, // コンテナウィンドウに立体枠を付けたいならこっちを使う
		0, 0, 100, 100,
		g_targetDialog,
		0,
		g_instance,
		0);
	_RPTF_HEX(g_container);
	SetWindowFont(g_container, font, FALSE);

	// ラベル (スタティックテキスト) を作成する。
	g_label = ::CreateWindowEx(
		0,
		_T("Static"),
		_T("下がフォルダだからね！勘違いしないでよね！"),
		WS_CHILD | WS_VISIBLE |
		SS_CENTER | SS_CENTERIMAGE,
		0, 0, 100, 100,
		g_container,
		(HMENU)1000,
		g_instance,
		0);
	_RPTF_HEX(g_label);
	SetWindowFont(g_label, font, FALSE);

	// エイリアスフォルダのリスト (コンボボックス) を作成する。
	g_aliasFolderList = ::CreateWindowEx(
		0,
		_T("ComboBox"),
		_T(""),
		WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL |
//		CBS_AUTOHSCROLL | // これは何の作用があるのか分からない
//		CBS_DISABLENOSCROLL | // ドロップダウンリストのスクロールバーを強制表示するならこれも使う
		CBS_DROPDOWNLIST | CBS_HASSTRINGS,
		0, 0, 100, 100,
		g_container,
		(HMENU)1001,
		g_instance,
		0);
	_RPTF_HEX(g_aliasFolderList);
	SetWindowFont(g_aliasFolderList, font, FALSE);

	// エイリアスフォルダリストを構築する。
	constructAliasFolderList();

	_RPTF_STR(g_lastAliasFolderPath);

	// 最後に選択したエイリアスフォルダをエディットボックスに入れる。
	HWND aliasFolder = ::GetDlgItem(g_targetDialog, ID_ALIAS_FOLDER);
	::SetWindowText(aliasFolder, g_lastAliasFolderPath);

	// 最後に選択したエイリアスフォルダをコンボボックスにも入れる。
	setCurrentText(g_aliasFolderList, g_lastAliasFolderPath);
}

// コンテナウィンドウを削除する
void destroyContainer()
{
	_RPTFT0(_T("destroyContainer()\n"));

	::DestroyWindow(g_container);
}

// コンテナウィンドウとコントロール群の表示位置を調整し、表示する
void showContainer()
{
	_RPTFT0(_T("showContainer()\n"));

	// AviUtl (exedit) 側のコントロールを基準にする。
	HWND aliasFolder = ::GetDlgItem(g_targetDialog, ID_ALIAS_FOLDER);
	RECT rcBase; ::GetWindowRect(aliasFolder, &rcBase);
	int rcBaseWidth = GetWidth(&rcBase);
	int rcBaseHeight = GetHeight(&rcBase) + 4; // コントロールの高さが低すぎるので若干盛る

	{
		int x = rcBase.right;
		int y = rcBase.top - rcBaseHeight;
		int w = rcBaseWidth;
		int h = rcBaseHeight * 2;

		SetClientRect(g_container, x, y, w, h);
	}

	HDWP dwp = ::BeginDeferWindowPos(2); // 引数は動かすウィンドウの数
	_RPTF_HEX(dwp);
	if(!dwp) return;

	{
		int x = 0;
		int y = 0;
		int w = rcBaseWidth;
		int h = rcBaseHeight;

		dwp = ::DeferWindowPos(dwp, g_label, NULL,
			x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
		_RPTF_HEX(dwp);
		if(!dwp) return;
	}

	{
		int x = 0;
		int y = rcBaseHeight;
		int w = rcBaseWidth;
		int h = rcBaseHeight * 20;

//		::SendMessage(g_aliasFolderList, CB_SETDROPPEDWIDTH, w * 2, 0); // ドロップダウンリストの幅は広めにするならこれを使う
//		::SendMessage(g_aliasFolderList, CB_SETHORIZONTALEXTENT, w, 0); // 水平スクロールバーを使用するならこれを使う
		dwp = ::DeferWindowPos(dwp, g_aliasFolderList, NULL,
			x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
		_RPTF_HEX(dwp);
		if(!dwp) return;
	}

	BOOL result = ::EndDeferWindowPos(dwp);
	_RPTF_NUM(result);

	::ShowWindow(g_container, SW_SHOWNA);
}

// コンテナウィンドウを非表示にする
void hideContainer()
{
	_RPTFT0(_T("hideContainer()\n"));

	::ShowWindow(g_container, SW_HIDE);
}

//---------------------------------------------------------------------

LRESULT CALLBACK container_wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_DESTROY:
		{
			_RPTFTN(_T("container_wndProc(WM_DESTROY, 0x%08X, 0x%08X)\n"), wParam, lParam);

			// コンテナウィンドウが削除されるときここに来る。

			// ここで後始末処理を行う。

			// 最後に使ったフォルダ名を保存する。
			HWND aliasFolder = ::GetDlgItem(g_targetDialog, ID_ALIAS_FOLDER);
			::GetWindowText(aliasFolder, g_lastAliasFolderPath, MAX_PATH);
			_RPTF_STR(g_lastAliasFolderPath);

			// コントロール群の削除は OS に任せる。ここでは変数を 0 に戻すだけでよい。
			g_targetDialog = 0;
			g_container = 0;
			g_label = 0;
			g_aliasFolderList = 0;

			break;
		}
#if 1
	case WM_MOUSEACTIVATE:
		{
			// コンテナウィンドウをクリックしたときここに来る。

			// コンテナウィンドウをクリックしてもアクティブにならないようにする。
			return MA_NOACTIVATE;
		}
#endif
	case WM_COMMAND:
		{
			_RPTFTN(_T("container_wndProc(WM_COMMAND, 0x%08X, 0x%08X)\n"), wParam, lParam);

			UINT code = HIWORD(wParam);
			UINT id = LOWORD(wParam);
			HWND sender = (HWND)lParam;

			if (code == LBN_SELCHANGE && sender == g_aliasFolderList)
			{
				// コンボボックスでフォルダを選択したときここに来る。

				_RPTFT0(_T("エディットボックスにエイリアスフォルダ名を入れます\n"));

				HWND aliasName = ::GetDlgItem(g_targetDialog, ID_ALIAS_NAME);
				HWND aliasFolder = ::GetDlgItem(g_targetDialog, ID_ALIAS_FOLDER);

				tstring text = getCurrentText(sender);
				::SetWindowText(aliasFolder, text.c_str());
				Edit_SetSel(aliasFolder, 0, -1);
				::SetFocus(aliasName);
//				::SetFocus(aliasFolder); // フォルダの方にフォーカスを当てるならこっちを使う
			}

			break;
		}
	}

	return ::DefWindowProc(hwnd, message, wParam, lParam);
}

//---------------------------------------------------------------------

// CallWndProcRet フック関数
LRESULT CALLBACK cwprProc(int code, WPARAM wParam, LPARAM lParam)
{
	if (code >= 0)
	{
		CWPRETSTRUCT* cwpr = (CWPRETSTRUCT*)lParam;

		switch(cwpr->message)
		{
		case WM_INITDIALOG:
			{
				// このウィンドウのタイトルを取得する。
				TCHAR windowText[MAX_PATH] = {};
				::GetWindowText(cwpr->hwnd, windowText, MAX_PATH);

				if (::StrStr(windowText, _T("エイリアスの作成")))
				{
					// "エイリアスの作成" ダイアログの初期化が終わったので、
					// このプラグイン用コントロールを格納するウィンドウを作成する。
					createContainer(cwpr->hwnd);
					showContainer();
				}

				break;
			}
#if 0
		case WM_DESTROY:
			{
				if (cwpr->hwnd == g_targetDialog)
				{
					_RPTFT0(_T("WM_DESTROY\n"));

					// コンテナウィンドウを削除する。
					// (自然に削除されるので、この処理は必要ない)
					hideContainer();
					destroyContainer();
				}

				break;
			}
#endif
		case WM_WINDOWPOSCHANGED:
			{
				if (cwpr->hwnd == g_targetDialog)
				{
					_RPTFT0(_T("WM_WINDOWPOSCHANGED\n"));

					// コンテナウィンドウをターゲットダイアログに追従させる。
					showContainer();
				}

				break;
			}
		}
	}

	return ::CallNextHookEx(g_hook, code, wParam, lParam);
}

// CallWndProcRet フックを作成する
void hook()
{
	_RPTFT0(_T("hook()\n"));

	g_hook = ::SetWindowsHookEx(WH_CALLWNDPROCRET, cwprProc, 0, ::GetCurrentThreadId());
}

// CallWndProcRet フックを削除する
void unhook()
{
	_RPTFT0(_T("unhook()\n"));

	::UnhookWindowsHookEx(g_hook), g_hook = 0;
}

//---------------------------------------------------------------------
//		初期化
//---------------------------------------------------------------------

BOOL func_init(FILTER *fp)
{
	// ロケールを設定する。これをやらないと日本語テキストが文字化けする。
	_tsetlocale(LC_ALL, _T(""));

	_RPTFT0(_T("func_init()\n"));

	// 色んな所で使う可能性があるので、
	// この DLL のハンドルをグローバル変数に保存しておく。
	g_instance = fp->dll_hinst;

	// CallWndProcRet フックを開始する。
	hook();

	return TRUE;
}

//---------------------------------------------------------------------
//		終了
//---------------------------------------------------------------------
BOOL func_exit(FILTER *fp)
{
	_RPTFT0(_T("func_term()\n"));

	// CallWndProcRet フックを終了する。
	unhook();

	return TRUE;
}
