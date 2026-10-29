#include "store_window.h"

#include "config.h"
#include "main.h"
#include "native_packet.h"
#include "obfuscated_string.h"
#include "security_build_config.h"
#include "timer.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace
{
	constexpr BYTE RESPONSE_ERROR = 0x00;
	constexpr BYTE RESPONSE_CATALOG = 0x01;
	constexpr BYTE RESPONSE_SUCCESS = 0x02;
	constexpr BYTE RESPONSE_HISTORY = 0x03;

	// Proportions used by the native 8.70 Store window reference.
	constexpr int WINDOW_WIDTH = 605;
	constexpr int WINDOW_HEIGHT = 404;
	constexpr int CATEGORY_ROW_HEIGHT = 35;
	constexpr int OFFER_ROW_HEIGHT = 35;
	constexpr int HISTORY_ROW_HEIGHT = 30;
	constexpr int SCROLLBAR_WIDTH = 12;
	constexpr std::size_t VISIBLE_CATEGORIES = 7;
	constexpr std::size_t VISIBLE_OFFERS = 6;
	constexpr std::size_t VISIBLE_HISTORY = 7;
	constexpr int SKIN_SCROLL_TRACK_VERTICAL = 153;
	constexpr int SKIN_SCROLL_THUMB_TOP = 154;
	constexpr int SKIN_SCROLL_THUMB_MIDDLE = 155;
	constexpr int SKIN_SCROLL_THUMB_BOTTOM = 156;
	constexpr int SKIN_SCROLL_UP = 157;
	constexpr int SKIN_SCROLL_UP_PRESSED = 158;
	constexpr int SKIN_SCROLL_DOWN = 159;
	constexpr int SKIN_SCROLL_DOWN_PRESSED = 160;
	constexpr std::size_t MAX_CATEGORIES = 128;
	constexpr std::size_t MAX_OFFERS_PER_CATEGORY = 4096;
	constexpr std::size_t MAX_TOTAL_OFFERS = 10000;
	constexpr std::size_t MAX_HISTORY = 256;
	constexpr std::size_t MAX_STORE_STRING_LENGTH = 4096;
	constexpr std::size_t MAX_STORE_BANNERS = 32;

	enum ButtonId
	{
		BUTTON_NONE = 0,
		BUTTON_BUY_POINTS,
		BUTTON_HISTORY,
		BUTTON_BUY,
		BUTTON_CLOSE,
		BUTTON_CATEGORY_UP,
		BUTTON_CATEGORY_DOWN,
		BUTTON_OFFER_UP,
		BUTTON_OFFER_DOWN,
		BUTTON_CATEGORY_THUMB,
		BUTTON_OFFER_THUMB
	};

	using WriteValueFn = void (__cdecl*)(DWORD);
	using DrawItemFn = void (__cdecl*)(int, int, int, int, int, int, int,
		int, int, int, int, int, int, int, int, int, int, int, int, char);
	using DrawRectangleFn = void (__fastcall*)(DWORD, DWORD, DWORD, DWORD,
		DWORD, DWORD, DWORD, DWORD, DWORD, DWORD);

	struct Rect
	{
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;

		bool Contains(int px, int py) const noexcept
		{
			return px >= x && py >= y && px < x + width && py < y + height;
		}
	};

	struct Layout
	{
		Rect window;
		Rect points;
		Rect buyPoints;
		Rect categories;
		Rect offers;
		Rect details;
		Rect historyButton;
		Rect buyButton;
		Rect closeButton;
	};

	struct Offer
	{
		DWORD id = 0;
		std::string name;
		std::string icon;
		DWORD price = 0;
		WORD displayId = 0;
		WORD count = 0;
		std::string description;
		std::string type;
	};

	struct Category
	{
		std::string name;
		std::string icon;
		std::string parent;
		std::string description;
		std::vector<Offer> offers;
	};

	struct HistoryEntry
	{
		std::string date;
		DWORD price = 0;
		bool credit = false;
		bool secondCurrency = false;
		std::string title;
		WORD count = 0;
	};

	struct StoreCatalog
	{
		DWORD balance = 0;
		std::vector<Category> categories;
		std::uint64_t fingerprint = 0;
	};

	struct StoreState
	{
		bool hasData = false;
		bool visible = false;
		bool loading = false;
		bool historyMode = false;
		std::shared_ptr<const StoreCatalog> catalog;
		std::shared_ptr<const std::vector<HistoryEntry>> history;
		std::size_t selectedCategory = 0;
		std::size_t selectedOffer = 0;
		std::size_t categoryOffset = 0;
		std::size_t offerOffset = 0;
		std::size_t historyOffset = 0;
		std::string status;
		bool statusError = false;
		int pressedButton = BUTTON_NONE;
		int draggedScrollbar = BUTTON_NONE;
		int scrollbarGrabOffset = 0;
	};

	using NativePacket = NativePacketBuffer::Packet;

	BYTE StoreHistoryOpcode() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0xFA);
		return value[0];
	}

	BYTE StoreBuyOpcode() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0xFC);
		return value[0];
	}

	WriteValueFn g_beginPacket = nullptr;
	WriteValueFn g_addU32 = nullptr;
	WriteValueFn g_sendPacket = nullptr;
	DrawItemFn g_drawItem = nullptr;
	DWORD g_networkBufferAddress = 0;
	DWORD g_playerIdAddress = 0;
	DWORD g_clientBaseAddress = 0;
	DWORD g_sessionPlayerId = 0;
	std::mutex g_mutex;
	StoreState g_state;
	std::atomic<bool> g_visible(false);
	HWND g_clientWindow = nullptr;
	WNDPROC g_originalWndProc = nullptr;

	const StoreCatalog& Catalog(const StoreState& state) noexcept
	{
		static const StoreCatalog empty;
		return state.catalog ? *state.catalog : empty;
	}

	const std::vector<Category>& Categories(const StoreState& state) noexcept
	{
		return Catalog(state).categories;
	}

	const std::vector<HistoryEntry>& History(const StoreState& state) noexcept
	{
		static const std::vector<HistoryEntry> empty;
		return state.history ? *state.history : empty;
	}

	#if TIBIA_ENABLE_DEV_LOGS
	void StoreLog(const char* format, ...) noexcept
	{
#ifdef __CONFIG__
		if(!should_debug_store || !format)
			return;
#else
		(void)format;
		return;
#endif
		char message[512] = {};
		va_list arguments;
		va_start(arguments, format);
		_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
		va_end(arguments);
		OutputDebugStringA("[Store] ");
		OutputDebugStringA(message);
		OutputDebugStringA("\n");
	}
	#else
	#define StoreLog(...) ((void)0)
	#endif

	bool ReadPlayerId(DWORD& playerId) noexcept
	{
		playerId = 0;
		if(!g_playerIdAddress)
			return false;
		__try {
			playerId = *reinterpret_cast<const DWORD*>(g_playerIdAddress);
			return playerId != 0;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			playerId = 0;
			return false;
		}
	}

	class PacketReader
	{
	public:
		PacketReader(const BYTE* data, std::size_t size) noexcept :
			data_(data), size_(size) {}

		bool ReadU8(BYTE& value) noexcept
		{
			if(!CanRead(1))
				return false;
			value = data_[position_++];
			return true;
		}

		bool ReadU16(WORD& value) noexcept
		{
			if(!CanRead(2))
				return false;
			value = static_cast<WORD>(data_[position_]) |
				static_cast<WORD>(data_[position_ + 1] << 8);
			position_ += 2;
			return true;
		}

		bool ReadU32(DWORD& value) noexcept
		{
			if(!CanRead(4))
				return false;
			value = static_cast<DWORD>(data_[position_]) |
				(static_cast<DWORD>(data_[position_ + 1]) << 8) |
				(static_cast<DWORD>(data_[position_ + 2]) << 16) |
				(static_cast<DWORD>(data_[position_ + 3]) << 24);
			position_ += 4;
			return true;
		}

		bool ReadString(std::string& value, std::size_t maximumLength)
		{
			WORD length = 0;
			if(!ReadU16(length) || length > maximumLength || !CanRead(length))
				return false;
			value.assign(reinterpret_cast<const char*>(data_ + position_), length);
			position_ += length;
			return true;
		}

		std::size_t Position() const noexcept { return position_; }

	private:
		bool CanRead(std::size_t count) const noexcept
		{
			return count <= size_ - (std::min)(position_, size_);
		}

		const BYTE* data_ = nullptr;
		std::size_t size_ = 0;
		std::size_t position_ = 0;
	};

	void DiscardNativePacketRemainder(const NativePacket& packet) noexcept
	{
		if(!g_networkBufferAddress)
			return;
		__try {
			DWORD* native = reinterpret_cast<DWORD*>(g_networkBufferAddress);
			if(reinterpret_cast<const BYTE*>(native[0]) == packet.data &&
				native[1] == packet.size && native[2] == packet.start)
				native[2] = native[1];
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool CopyNativePacketRemainder(const NativePacket& packet, BYTE* destination,
		std::size_t size) noexcept
	{
		if(!destination || size != packet.size - packet.start)
			return false;
		__try {
			std::memcpy(destination, packet.data + packet.start, size);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	std::uint64_t Fingerprint(const BYTE* data, std::size_t size) noexcept
	{
		std::uint64_t value = 1469598103934665603ULL;
		for(std::size_t index = 0; index < size; ++index)
		{
			value ^= data[index];
			value *= 1099511628211ULL;
		}
		return value;
	}

	struct ParsedResponse
	{
		BYTE type = 0xFF;
		std::shared_ptr<StoreCatalog> catalog;
		std::shared_ptr<std::vector<HistoryEntry>> history;
		DWORD offerId = 0;
		DWORD balance = 0;
		std::string message;
	};

	bool HandleClientMouseMessage(WPARAM message, int x, int y,
		short wheelDelta) noexcept;
	LRESULT CALLBACK StoreWindowProc(HWND window, UINT message,
		WPARAM wParam, LPARAM lParam);
	void DetachInputWindow() noexcept;

	HWND ClientWindow() noexcept
	{
		if(g_clientWindow && IsWindow(g_clientWindow))
			return g_clientWindow;
		g_clientWindow = nullptr;

		HWND window = GetForegroundWindow();
		if(!window)
			return nullptr;

		DWORD processId = 0;
		GetWindowThreadProcessId(window, &processId);
		if(processId != GetCurrentProcessId())
			return nullptr;
		g_clientWindow = window;
		return window;
	}

	bool EnsureInputWindow() noexcept
	{
		if(g_originalWndProc)
		{
			if(g_clientWindow && IsWindow(g_clientWindow))
				return true;
			DetachInputWindow();
		}
		HWND window = ClientWindow();
		if(!window)
			return false;

		SetLastError(ERROR_SUCCESS);
		const LONG_PTR previous = SetWindowLongPtrA(window, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(&StoreWindowProc));
		if(previous == 0 && GetLastError() != ERROR_SUCCESS)
			return false;
		g_originalWndProc = reinterpret_cast<WNDPROC>(previous);
		if(!g_originalWndProc)
			return false;
		return true;
	}

	void DetachInputWindow() noexcept
	{
		if(g_clientWindow && g_originalWndProc && IsWindow(g_clientWindow))
		{
			const LONG_PTR current = GetWindowLongPtrA(g_clientWindow, GWLP_WNDPROC);
			if(current == reinterpret_cast<LONG_PTR>(&StoreWindowProc))
				SetWindowLongPtrA(g_clientWindow, GWLP_WNDPROC,
					reinterpret_cast<LONG_PTR>(g_originalWndProc));
		}
		g_originalWndProc = nullptr;
		g_clientWindow = nullptr;
	}

	LRESULT CALLBACK StoreWindowProc(HWND window, UINT message,
		WPARAM wParam, LPARAM lParam)
	{
		if(StoreWindow::IsVisible())
		{
			if(message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
				message == WM_MOUSEMOVE)
			{
				const int x = static_cast<short>(LOWORD(lParam));
				const int y = static_cast<short>(HIWORD(lParam));
				if(HandleClientMouseMessage(message, x, y, 0))
					return 0;
			}
			else if(message == WM_MOUSEWHEEL)
			{
				POINT point = {static_cast<short>(LOWORD(lParam)),
					static_cast<short>(HIWORD(lParam))};
				ScreenToClient(window, &point);
				if(HandleClientMouseMessage(message, point.x, point.y,
					static_cast<short>(HIWORD(wParam))))
					return 0;
			}
			else if(message == WM_KEYDOWN && wParam == VK_ESCAPE)
			{
				StoreWindow::Close();
				return 0;
			}
		}

		return g_originalWndProc ?
			CallWindowProcA(g_originalWndProc, window, message, wParam, lParam) :
			DefWindowProcA(window, message, wParam, lParam);
	}

	bool IsOnline() noexcept
	{
		DWORD playerId = 0;
		return ReadPlayerId(playerId);
	}

	Layout BuildLayout(HWND window) noexcept
	{
		RECT client = {0, 0, 800, 600};
		if(window)
			GetClientRect(window, &client);

		const int clientWidth = std::max<int>(client.right - client.left,
			WINDOW_WIDTH);
		const int clientHeight = std::max<int>(client.bottom - client.top,
			WINDOW_HEIGHT);
		const int x = (clientWidth - WINDOW_WIDTH) / 2;
		const int y = (clientHeight - WINDOW_HEIGHT) / 2;

		Layout layout;
		layout.window = {x, y, WINDOW_WIDTH, WINDOW_HEIGHT};
		layout.points = {x + 14, y + 30, 190, 64};
		layout.buyPoints = {x + 66, y + 61, 86, 20};
		layout.categories = {x + 14, y + 101, 190, 250};
		layout.offers = {x + 220, y + 30, 365, 218};
		layout.details = {x + 220, y + 255, 365, 96};
		layout.historyButton = {x + 66, y + 366, 86, 20};
		layout.buyButton = {x + 405, y + 366, 62, 20};
		layout.closeButton = {x + 536, y + 366, 55, 20};
		return layout;
	}

	void SafeDrawRectangle(int surface, int x, int y, int width, int height,
		int red, int green, int blue) noexcept
	{
		if(width <= 0 || height <= 0 || !GetEngineAddr)
			return;
		__try {
			const DWORD engine = GetEngineAddr();
			if(!engine)
				return;
			const DWORD table = *reinterpret_cast<const DWORD*>(engine);
			if(!table)
				return;
			const DWORD function = *reinterpret_cast<const DWORD*>(table + 0x14);
			if(!function)
				return;
			reinterpret_cast<DrawRectangleFn>(function)(engine, 0, surface,
				static_cast<DWORD>(x), static_cast<DWORD>(y),
				static_cast<DWORD>(width), static_cast<DWORD>(height),
				static_cast<DWORD>(red), static_cast<DWORD>(green),
				static_cast<DWORD>(blue));
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void SafeDrawSkin(int surface, int x, int y, int width, int height,
		int skin) noexcept
	{
		if(!DrawSkin || width <= 0 || height <= 0)
			return;
		__try {
			DrawSkin(surface, x, y, width, height, skin, 0, 0);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void SafePrint(int surface, int x, int y, int red, int green, int blue,
		const char* text, int align = 0) noexcept
	{
		if(!PrintText || !text)
			return;
		__try {
			PrintText(surface, x, y, 2, red, green, blue, text, align);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void NativeTextColor(int& red, int& green, int& blue) noexcept
	{
		red = 224;
		green = 224;
		blue = 224;
		if(!g_clientBaseAddress)
			return;
		__try {
			red = static_cast<int>(*reinterpret_cast<const DWORD*>(
				g_clientBaseAddress + 0x24AAA4) & 0xFF);
			green = static_cast<int>(*reinterpret_cast<const DWORD*>(
				g_clientBaseAddress + 0x24AAA8) & 0xFF);
			blue = static_cast<int>(*reinterpret_cast<const DWORD*>(
				g_clientBaseAddress + 0x24AAAC) & 0xFF);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			red = green = blue = 224;
		}
	}

	void SafeDrawItem(int surface, int x, int y, WORD clientId) noexcept
	{
		if(!g_drawItem || clientId == 0)
			return;
		__try {
			g_drawItem(surface, x, y, 32, clientId, 0, 0, 0, 0, 0,
				0, 0, 0xFFFF, 0xFFFF, 2, 191, 191, 191, 2, 0);
		} __except(EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	void DrawPanel(int surface, const Rect& rect) noexcept
	{
		// Exact CIP ListBox/MemoBox composition: RGB 64 content and borders
		// from Tibia.pic. The surrounding modal keeps skin 7's grey texture.
		SafeDrawRectangle(surface, rect.x, rect.y, rect.width, rect.height,
			64, 64, 64);
		SafeDrawSkin(surface, rect.x - 1, rect.y - 1, rect.width + 2, 1, 40);
		SafeDrawSkin(surface, rect.x - 1, rect.y - 1, 1, rect.height + 1, 47);
		SafeDrawSkin(surface, rect.x - 1, rect.y + rect.height,
			rect.width + 2, 1, 41);
		SafeDrawSkin(surface, rect.x + rect.width, rect.y,
			1, rect.height, 47);
	}

	void DrawGreenButton(int surface, const Rect& rect, const char* caption,
		bool pressed) noexcept
	{
		const int offset = pressed ? 1 : 0;
		SafeDrawRectangle(surface, rect.x, rect.y, rect.width, rect.height,
			24, 45, 24);
		SafeDrawRectangle(surface, rect.x + 1, rect.y + 1,
			rect.width - 2, rect.height - 2,
			pressed ? 55 : 64, pressed ? 118 : 142, pressed ? 55 : 64);
		SafeDrawRectangle(surface, rect.x + 1, rect.y + 1,
			rect.width - 2, 1, pressed ? 80 : 116, pressed ? 154 : 187,
			pressed ? 80 : 116);
		SafePrint(surface, rect.x + rect.width / 2 + offset,
			rect.y + rect.height / 2 - 3 + offset,
			245, 245, 245, caption, 1);
	}

	void DrawButton(int surface, const Rect& rect, const char* caption,
		bool enabled = true, bool pressed = false) noexcept
	{
		const int offset = enabled && pressed ? 1 : 0;
		int red = 224;
		int green = 224;
		int blue = 224;
		NativeTextColor(red, green, blue);
		if(enabled)
		{
			red = (std::max)(red, 240);
			green = (std::max)(green, 240);
			blue = (std::max)(blue, 240);
		}
		else
			red = green = blue = 125;
		SafeDrawSkin(surface, rect.x, rect.y, rect.width, rect.height,
			enabled && pressed ? 19 : 18);
		SafePrint(surface, rect.x + rect.width / 2 + offset,
			rect.y + rect.height / 2 - 3 + offset,
			red, green, blue, caption, 1);
	}

	std::string Shorten(const std::string& value, std::size_t maximum)
	{
		if(value.size() <= maximum)
			return value;
		if(maximum <= 3)
			return value.substr(0, maximum);
		return value.substr(0, maximum - 3) + "...";
	}

	std::vector<std::string> Wrap(const std::string& value,
		std::size_t width, std::size_t maximumLines)
	{
		std::vector<std::string> lines;
		std::size_t position = 0;
		while(position < value.size() && lines.size() < maximumLines)
		{
			while(position < value.size() && value[position] == ' ')
				++position;
			if(position >= value.size())
				break;
			std::size_t end = (std::min)(value.size(), position + width);
			if(end < value.size())
			{
				const std::size_t separator = value.rfind(' ', end);
				if(separator != std::string::npos && separator > position)
					end = separator;
			}
			lines.push_back(value.substr(position, end - position));
			position = end;
		}
		if(position < value.size() && !lines.empty())
			lines.back() = Shorten(lines.back(), width);
		return lines;
	}

	bool IsDrawableItemType(const std::string& type) noexcept
	{
		return type.empty() || _stricmp(type.c_str(), "item") == 0 ||
			_stricmp(type.c_str(), "house") == 0;
	}

	const Offer* SelectedOffer(const StoreState& state) noexcept
	{
		const std::vector<Category>& categories = Categories(state);
		if(state.selectedCategory >= categories.size())
			return nullptr;
		const Category& category = categories[state.selectedCategory];
		return state.selectedOffer < category.offers.size() ?
			&category.offers[state.selectedOffer] : nullptr;
	}

	bool OfferNeedsExtraInput(const Offer& offer) noexcept
	{
		return _stricmp(offer.type.c_str(), "changename") == 0 ||
			_stricmp(offer.type.c_str(), "hireling") == 0;
	}

	WORD CategoryDisplayId(const Category& category) noexcept
	{
		for(const Offer& offer : category.offers)
		{
			if(offer.displayId != 0 && IsDrawableItemType(offer.type))
				return offer.displayId;
		}
		return 0;
	}

	Rect ScrollbarRect(const Rect& panel) noexcept
	{
		// Match the native CIP ListBox: the 12-pixel scrollbar occupies the
		// right-most part of the content rectangle from top to bottom.
		return {panel.x + panel.width - SCROLLBAR_WIDTH,
			panel.y, SCROLLBAR_WIDTH, panel.height};
	}

	Rect ScrollbarThumbRect(const Rect& panel, std::size_t offset,
		std::size_t visible, std::size_t total) noexcept
	{
		if(visible == 0 || total <= visible)
			return {};
		const Rect scroll = ScrollbarRect(panel);
		const int trackY = scroll.y + SCROLLBAR_WIDTH;
		const int trackHeight = scroll.height - SCROLLBAR_WIDTH * 2;
		const int thumbHeight = (std::max)(SCROLLBAR_WIDTH,
			static_cast<int>(trackHeight * visible / total));
		const int travel = trackHeight - thumbHeight;
		const std::size_t maximumOffset = total - visible;
		const int thumbY = trackY + static_cast<int>(
			travel * offset / (std::max<std::size_t>)(maximumOffset, 1));
		return {scroll.x, thumbY, SCROLLBAR_WIDTH, thumbHeight};
	}

	void DrawScrollbar(int surface, const Rect& panel, std::size_t offset,
		std::size_t visible, std::size_t total, int upButton, int downButton,
		int pressedButton) noexcept
	{
		if(visible == 0)
			return;
		const Rect scroll = ScrollbarRect(panel);
		const int trackY = scroll.y + SCROLLBAR_WIDTH;
		const int trackHeight = scroll.height - SCROLLBAR_WIDTH * 2;
		SafeDrawSkin(surface, scroll.x, trackY, SCROLLBAR_WIDTH,
			trackHeight, SKIN_SCROLL_TRACK_VERTICAL);
		SafeDrawSkin(surface, scroll.x, scroll.y, SCROLLBAR_WIDTH,
			SCROLLBAR_WIDTH,
			pressedButton == upButton ? SKIN_SCROLL_UP_PRESSED : SKIN_SCROLL_UP);
		SafeDrawSkin(surface, scroll.x,
			scroll.y + scroll.height - SCROLLBAR_WIDTH,
			SCROLLBAR_WIDTH, SCROLLBAR_WIDTH,
			pressedButton == downButton ? SKIN_SCROLL_DOWN_PRESSED :
			SKIN_SCROLL_DOWN);

		// The native CIP/TFC control keeps track and arrows visible but omits
		// the thumb when every entry already fits.
		if(total <= visible)
			return;

		const Rect thumb = ScrollbarThumbRect(panel, offset, visible, total);
		const int thumbHeight = thumb.height;
		const int thumbY = thumb.y;
		SafeDrawSkin(surface, scroll.x, thumbY, SCROLLBAR_WIDTH, 6,
			SKIN_SCROLL_THUMB_TOP);
		if(thumbHeight > 12)
			SafeDrawSkin(surface, scroll.x, thumbY + 6, SCROLLBAR_WIDTH,
				thumbHeight - 12, SKIN_SCROLL_THUMB_MIDDLE);
		SafeDrawSkin(surface, scroll.x, thumbY + thumbHeight - 6,
			SCROLLBAR_WIDTH, 6, SKIN_SCROLL_THUMB_BOTTOM);
	}

	void DrawStore(int surface, const Layout& layout,
		const StoreState& state)
	{
		const StoreCatalog& catalog = Catalog(state);
		const std::vector<Category>& categories = catalog.categories;
		const std::vector<HistoryEntry>& history = History(state);
		const Rect& window = layout.window;
		const int frameWidth = window.width - 4;
		const int frameHeight = window.height - 4;
		int textRed = 224;
		int textGreen = 224;
		int textBlue = 224;
		NativeTextColor(textRed, textGreen, textBlue);
		SafeDrawRectangle(surface, window.x + 1, window.y + 1,
			frameWidth + 3, frameHeight + 3, 32, 32, 32);
		SafeDrawRectangle(surface, window.x + 4, window.y + 18,
			frameWidth - 3, frameHeight - 19, 56, 56, 56);
		SafeDrawSkin(surface, window.x + 2, window.y,
			frameWidth, frameHeight, 7);
		SafeDrawSkin(surface, window.x, window.y, 4, frameHeight, 47);
		SafeDrawSkin(surface, window.x + frameWidth + 1, window.y,
			4, frameHeight + 4, 48);
		SafeDrawSkin(surface, window.x + 4, window.y + frameHeight,
			frameWidth - 2, 4, 50);
		SafeDrawSkin(surface, window.x + 1, window.y,
			frameWidth + 2, 17, 49);
		SafeDrawSkin(surface, window.x, window.y + frameHeight, 4, 4, 45);
		SafeDrawSkin(surface, window.x + frameWidth + 1,
			window.y + frameHeight, 4, 4, 46);
		SafePrint(surface, window.x + window.width / 2, window.y + 5,
			143, 143, 143,
			state.historyMode ? "Store History" : "Store", 1);

		DrawPanel(surface, layout.points);
		char balance[64] = {};
		_snprintf_s(balance, sizeof(balance), _TRUNCATE, "Points: %lu C",
			static_cast<unsigned long>(catalog.balance));
		SafePrint(surface, layout.points.x + layout.points.width / 2,
			layout.points.y + 12, 232, 232, 232, balance, 1);
		DrawGreenButton(surface, layout.buyPoints, "Buy Points",
			state.pressedButton == BUTTON_BUY_POINTS);

		DrawPanel(surface, layout.categories);
		for(std::size_t row = 0; row < VISIBLE_CATEGORIES; ++row)
		{
			const std::size_t index = state.categoryOffset + row;
			if(index >= categories.size())
				break;
			const int rowY = layout.categories.y + 3 +
				static_cast<int>(row) * CATEGORY_ROW_HEIGHT;
			if(!state.historyMode && index == state.selectedCategory)
				SafeDrawRectangle(surface, layout.categories.x + 3, rowY,
					layout.categories.width - 18, CATEGORY_ROW_HEIGHT - 1,
					112, 112, 112);
			const WORD displayId = CategoryDisplayId(categories[index]);
			if(displayId != 0)
				SafeDrawItem(surface, layout.categories.x + 4, rowY + 1, displayId);
			else
			{
				SafeDrawRectangle(surface, layout.categories.x + 11, rowY + 10,
					13, 13, 121, 76, 24);
				SafeDrawRectangle(surface, layout.categories.x + 13, rowY + 12,
					9, 9, 218, 138, 33);
			}
			const std::string name = Shorten(categories[index].name, 23);
			SafePrint(surface, layout.categories.x + 40, rowY + 11,
				220, 220, 220, name.c_str());
		}
		DrawScrollbar(surface, layout.categories, state.categoryOffset,
			VISIBLE_CATEGORIES, categories.size(), BUTTON_CATEGORY_UP,
			BUTTON_CATEGORY_DOWN, state.pressedButton);

		DrawPanel(surface, layout.offers);
		if(state.loading)
		{
			SafePrint(surface, layout.offers.x + layout.offers.width / 2,
				layout.offers.y + 92, 240, 210, 80,
				"Waiting for server...", 1);
		}
		else if(state.historyMode)
		{
			for(std::size_t row = 0; row < VISIBLE_HISTORY; ++row)
			{
				const std::size_t index = state.historyOffset + row;
				if(index >= history.size())
					break;
				const HistoryEntry& entry = history[index];
				const int rowY = layout.offers.y + 3 +
					static_cast<int>(row) * HISTORY_ROW_HEIGHT;
				if((row & 1U) != 0)
					SafeDrawRectangle(surface, layout.offers.x + 3, rowY,
						layout.offers.width - 14, HISTORY_ROW_HEIGHT - 1,
						61, 61, 61);
				const std::string title = Shorten(entry.title, 29);
				SafePrint(surface, layout.offers.x + 8, rowY + 6,
					210, 210, 210, title.c_str());
				char price[48] = {};
				_snprintf_s(price, sizeof(price), _TRUNCATE, "%c%lu C",
					entry.credit ? '+' : '-',
					static_cast<unsigned long>(entry.price));
				SafePrint(surface, layout.offers.x + layout.offers.width - 12,
					rowY + 6, entry.credit ? 90 : 240,
					entry.credit ? 220 : 120, 90, price, 2);
			}
			DrawScrollbar(surface, layout.offers, state.historyOffset,
				VISIBLE_HISTORY, history.size(), BUTTON_OFFER_UP,
				BUTTON_OFFER_DOWN, state.pressedButton);
		}
		else
		{
			const Category* category = state.selectedCategory <
				categories.size() ? &categories[state.selectedCategory] : nullptr;
			if(category)
			{
				for(std::size_t row = 0; row < VISIBLE_OFFERS; ++row)
				{
					const std::size_t index = state.offerOffset + row;
					if(index >= category->offers.size())
						break;
					const Offer& offer = category->offers[index];
					const int rowY = layout.offers.y + 3 +
						static_cast<int>(row) * OFFER_ROW_HEIGHT;
					if(index == state.selectedOffer)
						SafeDrawRectangle(surface, layout.offers.x + 3, rowY,
							layout.offers.width - 18, OFFER_ROW_HEIGHT - 1,
							112, 112, 112);
					else if((row & 1U) != 0)
						SafeDrawRectangle(surface, layout.offers.x + 3, rowY,
							layout.offers.width - 18, OFFER_ROW_HEIGHT - 1,
							61, 61, 61);
					if(IsDrawableItemType(offer.type))
						SafeDrawItem(surface, layout.offers.x + 5, rowY + 1,
							offer.displayId);
					const std::string name = Shorten(offer.name, 30);
					SafePrint(surface, layout.offers.x + 42, rowY + 10,
						228, 228, 228, name.c_str());
					char price[40] = {};
					_snprintf_s(price, sizeof(price), _TRUNCATE, "%lu C",
						static_cast<unsigned long>(offer.price));
					SafePrint(surface, layout.offers.x + layout.offers.width - 12,
						rowY + 10, 236, 178, 62, price, 2);
				}
				DrawScrollbar(surface, layout.offers, state.offerOffset,
					VISIBLE_OFFERS, category->offers.size(), BUTTON_OFFER_UP,
					BUTTON_OFFER_DOWN, state.pressedButton);
			}
		}

		DrawPanel(surface, layout.details);
		const Offer* offer = state.historyMode ? nullptr : SelectedOffer(state);
		if(offer)
		{
			const Rect iconPanel = {layout.details.x + 8, layout.details.y + 8,
				64, 64};
			DrawPanel(surface, iconPanel);
			if(IsDrawableItemType(offer->type))
				SafeDrawItem(surface, iconPanel.x + 16, iconPanel.y + 16,
					offer->displayId);
			const int textX = layout.details.x + 82;
			const int textWidth = layout.details.width - 92;
			const std::string title = Shorten(offer->name, 38);
			SafePrint(surface, textX + textWidth / 2,
				layout.details.y + 8, 235, 235, 235, title.c_str(), 1);
			const std::vector<std::string> lines = Wrap(offer->description, 38, 4);
			for(std::size_t line = 0; line < lines.size(); ++line)
				SafePrint(surface, textX,
					layout.details.y + 29 + static_cast<int>(line) * 14,
					190, 190, 190, lines[line].c_str());
		}
		else if(state.historyMode)
		{
			SafePrint(surface, layout.details.x + 10, layout.details.y + 12,
				190, 190, 190, "The latest account transactions are shown above.");
		}

		if(!state.status.empty())
		{
			const std::string status = Shorten(state.status, 70);
			SafePrint(surface, window.x + window.width / 2, window.y + 351,
				state.statusError ? 255 : 110,
				state.statusError ? 100 : 230,
				state.statusError ? 100 : 110, status.c_str(), 1);
		}

		DrawButton(surface, layout.historyButton,
			state.historyMode ? "Catalog" : "History", true,
			state.pressedButton == BUTTON_HISTORY);
		const bool canBuy = offer && !OfferNeedsExtraInput(*offer) && !state.loading;
		DrawButton(surface, layout.buyButton, "Buy", canBuy,
			state.pressedButton == BUTTON_BUY);
		DrawButton(surface, layout.closeButton, "Close", true,
			state.pressedButton == BUTTON_CLOSE);
	}

	bool SendPacket(BYTE opcode, DWORD value, bool withValue) noexcept
	{
		if(!g_beginPacket || !g_sendPacket || (withValue && !g_addU32) ||
			!IsOnline())
			return false;
		__try {
			g_beginPacket(opcode);
			if(withValue)
				g_addU32(value);
			g_sendPacket(1);
			return true;
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	bool ReadString(PacketReader& reader, std::string& value,
		const char* field, std::string& error)
	{
		if(reader.ReadString(value, MAX_STORE_STRING_LENGTH))
			return true;
		error = TIBIA_DIAGNOSTIC_TEXT(
			"Packet truncated or Store string too long while reading ");
		error += field;
		return false;
	}

	bool ReadOffer(PacketReader& reader, Offer& offer, std::string& error)
	{
		return reader.ReadU32(offer.id) &&
			ReadString(reader, offer.name,
				TIBIA_DIAGNOSTIC_TEXT("offer name"), error) &&
			ReadString(reader, offer.icon,
				TIBIA_DIAGNOSTIC_TEXT("offer icon"), error) &&
			reader.ReadU32(offer.price) &&
			reader.ReadU16(offer.displayId) &&
			reader.ReadU16(offer.count) &&
			ReadString(reader, offer.description,
				TIBIA_DIAGNOSTIC_TEXT("offer description"), error) &&
			ReadString(reader, offer.type,
				TIBIA_DIAGNOSTIC_TEXT("offer type"), error);
	}

	bool ParseCatalog(PacketReader& reader, ParsedResponse& response,
		std::string& error)
	{
		auto catalog = std::make_shared<StoreCatalog>();
		WORD categoryCount = 0;
		if(!reader.ReadU32(catalog->balance) || !reader.ReadU16(categoryCount))
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Packet truncated while reading Store catalog header");
			return false;
		}
		if(categoryCount > MAX_CATEGORIES)
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Store category count exceeds limit");
			return false;
		}
		catalog->categories.reserve(categoryCount);
		std::size_t totalOffers = 0;
		for(WORD categoryIndex = 0; categoryIndex < categoryCount;
			++categoryIndex)
		{
			Category category;
			if(!ReadString(reader, category.name,
					TIBIA_DIAGNOSTIC_TEXT("category name"), error) ||
				!ReadString(reader, category.icon,
					TIBIA_DIAGNOSTIC_TEXT("category icon"), error) ||
				!ReadString(reader, category.parent,
					TIBIA_DIAGNOSTIC_TEXT("category parent"), error) ||
				!ReadString(reader, category.description,
					TIBIA_DIAGNOSTIC_TEXT("category description"), error))
				return false;

			WORD offerCount = 0;
			if(!reader.ReadU16(offerCount))
			{
				error = TIBIA_DIAGNOSTIC_TEXT(
					"Packet truncated while reading Store offer count");
				return false;
			}
			if(offerCount > MAX_OFFERS_PER_CATEGORY ||
				totalOffers > MAX_TOTAL_OFFERS - offerCount)
			{
				error = TIBIA_DIAGNOSTIC_TEXT(
					"Store offer count exceeds limit");
				return false;
			}
			totalOffers += offerCount;
			category.offers.reserve(offerCount);
			for(WORD offerIndex = 0; offerIndex < offerCount; ++offerIndex)
			{
				Offer offer;
				if(!ReadOffer(reader, offer, error))
				{
					if(error.empty())
						error = TIBIA_DIAGNOSTIC_TEXT(
							"Packet truncated while reading Store offer");
					return false;
				}
				category.offers.push_back(std::move(offer));
			}
			catalog->categories.push_back(std::move(category));
		}

		BYTE bannerCount = 0;
		if(!reader.ReadU8(bannerCount))
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Packet truncated while reading Store banner count");
			return false;
		}
		if(bannerCount > MAX_STORE_BANNERS)
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Store banner count exceeds limit");
			return false;
		}
		for(BYTE banner = 0; banner < bannerCount; ++banner)
		{
			std::string image;
			BYTE action = 0;
			DWORD target = 0;
			if(!ReadString(reader, image,
					TIBIA_DIAGNOSTIC_TEXT("banner image"), error) ||
				!reader.ReadU8(action) || !reader.ReadU32(target))
			{
				if(error.empty())
					error = TIBIA_DIAGNOSTIC_TEXT(
						"Packet truncated while reading Store banner");
				return false;
			}
			if(action > 4)
			{
				error = TIBIA_DIAGNOSTIC_TEXT(
					"Unknown Store banner action");
				return false;
			}
		}
		BYTE bannerDelay = 0;
		if(!reader.ReadU8(bannerDelay))
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Packet truncated while reading Store banner delay");
			return false;
		}
		response.catalog = std::move(catalog);
		return true;
	}

	bool ParseHistory(PacketReader& reader, ParsedResponse& response,
		std::string& error)
	{
		WORD count = 0;
		if(!reader.ReadU16(count))
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Packet truncated while reading Store history count");
			return false;
		}
		if(count > MAX_HISTORY)
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Store history count exceeds limit");
			return false;
		}
		auto history = std::make_shared<std::vector<HistoryEntry>>();
		history->reserve(count);
		for(WORD index = 0; index < count; ++index)
		{
			HistoryEntry entry;
			BYTE credit = 0;
			BYTE secondCurrency = 0;
			if(!ReadString(reader, entry.date,
					TIBIA_DIAGNOSTIC_TEXT("history date"), error) ||
				!reader.ReadU32(entry.price) || !reader.ReadU8(credit) ||
				!reader.ReadU8(secondCurrency) ||
				!ReadString(reader, entry.title,
					TIBIA_DIAGNOSTIC_TEXT("history title"), error) ||
				!reader.ReadU16(entry.count))
			{
				if(error.empty())
					error = TIBIA_DIAGNOSTIC_TEXT(
						"Packet truncated while reading Store history");
				return false;
			}
			if(credit > 1 || secondCurrency > 1)
			{
				error = TIBIA_DIAGNOSTIC_TEXT(
					"Invalid Store history flags");
				return false;
			}
			entry.credit = credit != 0;
			entry.secondCurrency = secondCurrency != 0;
			history->push_back(std::move(entry));
		}
		response.history = std::move(history);
		return true;
	}

	bool ParseResponse(PacketReader& reader, ParsedResponse& response,
		std::string& error)
	{
		if(!reader.ReadU8(response.type))
		{
			error = TIBIA_DIAGNOSTIC_TEXT(
				"Packet truncated before Store response type");
			return false;
		}
		switch(response.type)
		{
			case RESPONSE_CATALOG:
				return ParseCatalog(reader, response, error);
			case RESPONSE_HISTORY:
				return ParseHistory(reader, response, error);
			case RESPONSE_ERROR:
				return ReadString(reader, response.message,
					TIBIA_DIAGNOSTIC_TEXT("error message"), error);
			case RESPONSE_SUCCESS:
				if(!reader.ReadU32(response.offerId) ||
					!ReadString(reader, response.message,
						TIBIA_DIAGNOSTIC_TEXT("success message"), error) ||
					!reader.ReadU32(response.balance))
				{
					if(error.empty())
						error = TIBIA_DIAGNOSTIC_TEXT(
							"Packet truncated while reading Store success");
					return false;
				}
				return true;
			default:
				error = TIBIA_DIAGNOSTIC_TEXT(
					"Unknown Store response type");
				return false;
		}
	}

	void SetVisibleLocked(bool visible) noexcept
	{
		g_state.visible = visible;
		g_visible.store(visible, std::memory_order_release);
	}

	void ResetSessionLocked(DWORD playerId)
	{
		g_state = StoreState();
		g_visible.store(false, std::memory_order_release);
		g_sessionPlayerId = playerId;
	}

	void ApplyCatalogLocked(std::shared_ptr<StoreCatalog> catalog)
	{
		const StoreCatalog& previous = Catalog(g_state);
		if(g_state.hasData && previous.fingerprint == catalog->fingerprint)
		{
			g_state.loading = false;
			return;
		}

		std::string selectedCategoryName;
		DWORD selectedOfferId = 0;
		if(g_state.selectedCategory < previous.categories.size())
		{
			const Category& category = previous.categories[g_state.selectedCategory];
			selectedCategoryName = category.name;
			if(g_state.selectedOffer < category.offers.size())
				selectedOfferId = category.offers[g_state.selectedOffer].id;
		}

		g_state.catalog = std::move(catalog);
		g_state.hasData = true;
		const std::vector<Category>& categories = Categories(g_state);
		g_state.selectedCategory = 0;
		g_state.selectedOffer = 0;
		for(std::size_t index = 0; index < categories.size(); ++index)
		{
			if(categories[index].name == selectedCategoryName)
			{
				g_state.selectedCategory = index;
				break;
			}
		}
		if(g_state.selectedCategory < categories.size())
		{
			const std::vector<Offer>& offers =
				categories[g_state.selectedCategory].offers;
			for(std::size_t index = 0; index < offers.size(); ++index)
			{
				if(offers[index].id == selectedOfferId)
				{
					g_state.selectedOffer = index;
					break;
				}
			}
		}
		g_state.categoryOffset = g_state.selectedCategory >= VISIBLE_CATEGORIES ?
			g_state.selectedCategory - VISIBLE_CATEGORIES + 1 : 0;
		g_state.offerOffset = g_state.selectedOffer >= VISIBLE_OFFERS ?
			g_state.selectedOffer - VISIBLE_OFFERS + 1 : 0;
		g_state.loading = false;
		g_state.status = categories.empty() ?
			"The server returned an empty Store catalog." : "";
		g_state.statusError = categories.empty();
	}

	void ApplyParsedResponse(ParsedResponse&& response, DWORD playerId)
	{
		std::size_t categoryCount = 0;
		std::size_t offerCount = 0;
		std::size_t historyCount = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if(g_sessionPlayerId != 0 && g_sessionPlayerId != playerId)
				ResetSessionLocked(playerId);
			else if(g_sessionPlayerId == 0)
				g_sessionPlayerId = playerId;

			if(response.type == RESPONSE_CATALOG && response.catalog)
			{
				categoryCount = response.catalog->categories.size();
				for(const Category& category : response.catalog->categories)
					offerCount += category.offers.size();
				ApplyCatalogLocked(std::move(response.catalog));
			}
			else if(response.type == RESPONSE_HISTORY && response.history)
			{
				historyCount = response.history->size();
				g_state.history = std::move(response.history);
				g_state.historyMode = true;
				g_state.historyOffset = 0;
				g_state.loading = false;
				g_state.status.clear();
				g_state.statusError = false;
			}
			else if(response.type == RESPONSE_ERROR)
			{
				g_state.loading = false;
				g_state.status = std::move(response.message);
				g_state.statusError = true;
			}
			else if(response.type == RESPONSE_SUCCESS)
			{
				auto catalog = std::make_shared<StoreCatalog>(Catalog(g_state));
				catalog->balance = response.balance;
				catalog->fingerprint = 0;
				g_state.catalog = std::move(catalog);
				g_state.hasData = true;
				g_state.loading = false;
				g_state.status = std::move(response.message);
				g_state.statusError = false;
			}
		}

		if(categoryCount != 0 || offerCount != 0)
			StoreLog("Received snapshot: categories=%zu offers=%zu",
				categoryCount, offerCount);
		else if(response.type == RESPONSE_HISTORY)
			StoreLog("History updated: entries=%zu", historyCount);
		else if(response.type == RESPONSE_SUCCESS)
			StoreLog("Balance updated after offer %lu",
				static_cast<unsigned long>(response.offerId));
	}

	void SetMalformedPacketError(const std::string& error)
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_state.loading = false;
			g_state.status = "Invalid Store packet received.";
			g_state.statusError = true;
		}
		#if !TIBIA_ENABLE_DEV_LOGS
		(void)error;
		#endif
		StoreLog("Invalid packet: %s", error.c_str());
	}

	std::size_t ClampOffset(std::size_t offset, std::size_t visible,
		std::size_t total) noexcept
	{
		return total > visible ? (std::min)(offset, total - visible) : 0;
	}

	void SetOffsetFromScrollbarPoint(std::size_t& offset, std::size_t visible,
		std::size_t total, const Rect& scroll, int y) noexcept
	{
		if(total <= visible || visible == 0)
		{
			offset = 0;
			return;
		}
		const int trackHeight = scroll.height - SCROLLBAR_WIDTH * 2;
		const int thumbHeight = (std::max)(SCROLLBAR_WIDTH,
			static_cast<int>(trackHeight * visible / total));
		const int travel = trackHeight - thumbHeight;
		if(travel <= 0)
		{
			offset = 0;
			return;
		}
		const int requested = std::clamp(
			y - (scroll.y + SCROLLBAR_WIDTH) - thumbHeight / 2, 0, travel);
		const std::size_t maximumOffset = total - visible;
		offset = static_cast<std::size_t>((
			static_cast<unsigned long long>(requested) * maximumOffset +
			static_cast<unsigned long long>(travel / 2)) /
			static_cast<unsigned long long>(travel));
	}

	bool BeginScrollbarDrag(StoreState& state, const Layout& layout,
		int x, int y) noexcept
	{
		const std::vector<Category>& categories = Categories(state);
		const std::vector<HistoryEntry>& history = History(state);
		if(categories.size() > VISIBLE_CATEGORIES)
		{
			const Rect thumb = ScrollbarThumbRect(layout.categories,
				state.categoryOffset, VISIBLE_CATEGORIES, categories.size());
			if(thumb.Contains(x, y))
			{
				state.draggedScrollbar = BUTTON_CATEGORY_THUMB;
				state.scrollbarGrabOffset = y - thumb.y;
				return true;
			}
		}

		const std::size_t total = state.historyMode ? history.size() :
			(state.selectedCategory < categories.size() ?
				categories[state.selectedCategory].offers.size() : 0);
		const std::size_t visible = state.historyMode ? VISIBLE_HISTORY :
			VISIBLE_OFFERS;
		if(total > visible)
		{
			const std::size_t offset = state.historyMode ? state.historyOffset :
				state.offerOffset;
			const Rect thumb = ScrollbarThumbRect(layout.offers, offset,
				visible, total);
			if(thumb.Contains(x, y))
			{
				state.draggedScrollbar = BUTTON_OFFER_THUMB;
				state.scrollbarGrabOffset = y - thumb.y;
				return true;
			}
		}
		return false;
	}

	bool UpdateScrollbarDrag(StoreState& state, const Layout& layout,
		int y) noexcept
	{
		const std::vector<Category>& categories = Categories(state);
		const std::vector<HistoryEntry>& history = History(state);
		Rect panel;
		std::size_t visible = 0;
		std::size_t total = 0;
		std::size_t* offset = nullptr;
		if(state.draggedScrollbar == BUTTON_CATEGORY_THUMB)
		{
			panel = layout.categories;
			visible = VISIBLE_CATEGORIES;
			total = categories.size();
			offset = &state.categoryOffset;
		}
		else if(state.draggedScrollbar == BUTTON_OFFER_THUMB)
		{
			panel = layout.offers;
			visible = state.historyMode ? VISIBLE_HISTORY : VISIBLE_OFFERS;
			total = state.historyMode ? history.size() :
				(state.selectedCategory < categories.size() ?
					categories[state.selectedCategory].offers.size() : 0);
			offset = state.historyMode ? &state.historyOffset : &state.offerOffset;
		}
		else
			return false;

		if(!offset || visible == 0 || total <= visible)
			return true;
		const Rect scroll = ScrollbarRect(panel);
		const Rect thumb = ScrollbarThumbRect(panel, *offset, visible, total);
		const int trackY = scroll.y + SCROLLBAR_WIDTH;
		const int trackHeight = scroll.height - SCROLLBAR_WIDTH * 2;
		const int travel = trackHeight - thumb.height;
		if(travel <= 0)
		{
			*offset = 0;
			return true;
		}
		const int requested = std::clamp(y - trackY -
			state.scrollbarGrabOffset, 0, travel);
		const std::size_t maximumOffset = total - visible;
		*offset = static_cast<std::size_t>((
			static_cast<unsigned long long>(requested) * maximumOffset +
			static_cast<unsigned long long>(travel / 2)) /
			static_cast<unsigned long long>(travel));
		return true;
	}

	int HitButton(const Layout& layout, const StoreState& state,
		int x, int y) noexcept
	{
		(void)state;
		if(layout.buyPoints.Contains(x, y))
			return BUTTON_BUY_POINTS;
		if(layout.historyButton.Contains(x, y))
			return BUTTON_HISTORY;
		if(layout.buyButton.Contains(x, y))
			return BUTTON_BUY;
		if(layout.closeButton.Contains(x, y))
			return BUTTON_CLOSE;

		{
			const Rect scroll = ScrollbarRect(layout.categories);
			const Rect up = {scroll.x, scroll.y, SCROLLBAR_WIDTH,
				SCROLLBAR_WIDTH};
			const Rect down = {scroll.x,
				scroll.y + scroll.height - SCROLLBAR_WIDTH,
				SCROLLBAR_WIDTH, SCROLLBAR_WIDTH};
			if(up.Contains(x, y))
				return BUTTON_CATEGORY_UP;
			if(down.Contains(x, y))
				return BUTTON_CATEGORY_DOWN;
		}

		{
			const Rect scroll = ScrollbarRect(layout.offers);
			const Rect up = {scroll.x, scroll.y, SCROLLBAR_WIDTH,
				SCROLLBAR_WIDTH};
			const Rect down = {scroll.x,
				scroll.y + scroll.height - SCROLLBAR_WIDTH,
				SCROLLBAR_WIDTH, SCROLLBAR_WIDTH};
			if(up.Contains(x, y))
				return BUTTON_OFFER_UP;
			if(down.Contains(x, y))
				return BUTTON_OFFER_DOWN;
		}
		return BUTTON_NONE;
	}

	bool HandleScrollbarTrackClick(StoreState& state, const Layout& layout,
		int x, int y) noexcept
	{
		const std::vector<Category>& categories = Categories(state);
		const std::vector<HistoryEntry>& history = History(state);
		if(categories.size() > VISIBLE_CATEGORIES)
		{
			const Rect scroll = ScrollbarRect(layout.categories);
			if(scroll.Contains(x, y))
			{
				SetOffsetFromScrollbarPoint(state.categoryOffset,
					VISIBLE_CATEGORIES, categories.size(), scroll, y);
				return true;
			}
		}

		const std::size_t offerCount = state.historyMode ? history.size() :
			(state.selectedCategory < categories.size() ?
				categories[state.selectedCategory].offers.size() : 0);
		const std::size_t offerVisible = state.historyMode ? VISIBLE_HISTORY :
			VISIBLE_OFFERS;
		if(offerCount > offerVisible)
		{
			const Rect scroll = ScrollbarRect(layout.offers);
			if(scroll.Contains(x, y))
			{
				std::size_t& offset = state.historyMode ? state.historyOffset :
					state.offerOffset;
				SetOffsetFromScrollbarPoint(offset, offerVisible, offerCount,
					scroll, y);
				return true;
			}
		}
		return false;
	}

	bool HandleClientMouseMessage(WPARAM message, int x, int y,
		short wheelDelta) noexcept
	{
		if(!StoreWindow::IsVisible())
			return false;
		HWND window = ClientWindow();
		if(!window)
			return false;
		const Layout layout = BuildLayout(window);
		if(message == WM_MOUSEMOVE)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			return UpdateScrollbarDrag(g_state, layout, y);
		}
		if(message == WM_LBUTTONUP)
		{
			bool endedDrag = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				if(g_state.draggedScrollbar != BUTTON_NONE)
				{
					UpdateScrollbarDrag(g_state, layout, y);
					g_state.draggedScrollbar = BUTTON_NONE;
					g_state.scrollbarGrabOffset = 0;
					endedDrag = true;
				}
			}
			if(endedDrag)
			{
				ReleaseCapture();
				return true;
			}
		}
		if(!layout.window.Contains(x, y))
			return false;

		if(message == WM_MOUSEWHEEL)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const std::vector<Category>& categories = Categories(g_state);
			const std::vector<HistoryEntry>& history = History(g_state);
			if(g_state.historyMode && layout.offers.Contains(x, y))
			{
				if(wheelDelta < 0)
					++g_state.historyOffset;
				else if(g_state.historyOffset > 0)
					--g_state.historyOffset;
				g_state.historyOffset = ClampOffset(g_state.historyOffset,
					VISIBLE_HISTORY, history.size());
			}
			else if(layout.categories.Contains(x, y))
			{
				if(wheelDelta < 0)
					++g_state.categoryOffset;
				else if(g_state.categoryOffset > 0)
					--g_state.categoryOffset;
				g_state.categoryOffset = ClampOffset(g_state.categoryOffset,
					VISIBLE_CATEGORIES, categories.size());
			}
			else if(!g_state.historyMode && layout.offers.Contains(x, y) &&
				g_state.selectedCategory < categories.size())
			{
				const std::size_t count =
					categories[g_state.selectedCategory].offers.size();
				if(wheelDelta < 0)
					++g_state.offerOffset;
				else if(g_state.offerOffset > 0)
					--g_state.offerOffset;
				g_state.offerOffset = ClampOffset(g_state.offerOffset,
					VISIBLE_OFFERS, count);
			}
			return true;
		}

		if(message == WM_LBUTTONDOWN)
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const std::vector<Category>& categories = Categories(g_state);
			g_state.pressedButton = HitButton(layout, g_state, x, y);
			if(g_state.pressedButton == BUTTON_BUY &&
				(!SelectedOffer(g_state) || g_state.loading))
				g_state.pressedButton = BUTTON_NONE;

			if(g_state.pressedButton != BUTTON_NONE)
				return true;
			if(BeginScrollbarDrag(g_state, layout, x, y))
			{
				SetCapture(window);
				return true;
			}
			if(HandleScrollbarTrackClick(g_state, layout, x, y))
				return true;
			if(!g_state.historyMode && layout.categories.Contains(x, y))
			{
				const int relative = y - layout.categories.y - 3;
				if(relative >= 0)
				{
					const std::size_t index = g_state.categoryOffset +
						static_cast<std::size_t>(relative / CATEGORY_ROW_HEIGHT);
					if(index < categories.size())
					{
						g_state.selectedCategory = index;
						g_state.selectedOffer = 0;
						g_state.offerOffset = 0;
						g_state.status.clear();
					}
				}
			}
			else if(!g_state.historyMode && layout.offers.Contains(x, y) &&
				g_state.selectedCategory < categories.size())
			{
				const int relative = y - layout.offers.y - 3;
				if(relative >= 0)
				{
					const std::size_t index = g_state.offerOffset +
						static_cast<std::size_t>(relative / OFFER_ROW_HEIGHT);
					if(index < categories[g_state.selectedCategory].offers.size())
					{
						g_state.selectedOffer = index;
						g_state.status.clear();
					}
				}
			}
			return true;
		}

		if(message != WM_LBUTTONUP)
			return false;

		BYTE requestOpcode = 0;
		DWORD requestValue = 0;
		bool requestHasValue = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			const std::vector<Category>& categories = Categories(g_state);
			const std::vector<HistoryEntry>& history = History(g_state);
			const int pressed = g_state.pressedButton;
			g_state.pressedButton = BUTTON_NONE;
			if(pressed == BUTTON_NONE ||
				pressed != HitButton(layout, g_state, x, y))
				return true;

			if(pressed == BUTTON_CATEGORY_UP || pressed == BUTTON_CATEGORY_DOWN)
			{
				if(pressed == BUTTON_CATEGORY_UP && g_state.categoryOffset > 0)
					--g_state.categoryOffset;
				else if(pressed == BUTTON_CATEGORY_DOWN)
					++g_state.categoryOffset;
				g_state.categoryOffset = ClampOffset(g_state.categoryOffset,
					VISIBLE_CATEGORIES, categories.size());
				return true;
			}
			if(pressed == BUTTON_OFFER_UP || pressed == BUTTON_OFFER_DOWN)
			{
				std::size_t& offset = g_state.historyMode ? g_state.historyOffset :
					g_state.offerOffset;
				const std::size_t total = g_state.historyMode ? history.size() :
					(g_state.selectedCategory < categories.size() ?
						categories[g_state.selectedCategory].offers.size() : 0);
				const std::size_t visible = g_state.historyMode ? VISIBLE_HISTORY :
					VISIBLE_OFFERS;
				if(pressed == BUTTON_OFFER_UP && offset > 0)
					--offset;
				else if(pressed == BUTTON_OFFER_DOWN)
					++offset;
				offset = ClampOffset(offset, visible, total);
				return true;
			}

			if(pressed == BUTTON_CLOSE)
			{
				SetVisibleLocked(false);
				g_state.loading = false;
				return true;
			}
			if(pressed == BUTTON_BUY_POINTS)
			{
				g_state.status = "Buy Points is managed by the account website.";
				g_state.statusError = false;
				return true;
			}
			if(pressed == BUTTON_HISTORY)
			{
				if(g_state.historyMode)
				{
					g_state.historyMode = false;
					g_state.loading = false;
				}
				else
				{
					g_state.loading = true;
					requestOpcode = StoreHistoryOpcode();
				}
			}
			else if(pressed == BUTTON_BUY && !g_state.historyMode)
			{
				const Offer* offer = SelectedOffer(g_state);
				if(offer && !g_state.loading)
				{
					if(OfferNeedsExtraInput(*offer))
					{
						g_state.status = "This offer requires text input not available in this modal.";
						g_state.statusError = true;
					}
					else
					{
						g_state.loading = true;
						g_state.status = "Processing purchase...";
						g_state.statusError = false;
						requestOpcode = StoreBuyOpcode();
						requestValue = offer->id;
						requestHasValue = true;
					}
				}
			}
		}

		if(requestOpcode != 0 &&
			!SendPacket(requestOpcode, requestValue, requestHasValue))
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			g_state.loading = false;
			g_state.status = "Could not send the Store request.";
			g_state.statusError = true;
		}
		return true;
	}
}

namespace StoreWindow
{
	BYTE ServerOpcode() noexcept
	{
		auto value = TIBIA_OBF_BYTES(0xFD);
		return value[0];
	}

	bool Install(DWORD clientBase) noexcept
	{
		if(!clientBase)
			return false;
		g_beginPacket = reinterpret_cast<WriteValueFn>(clientBase + 0xF8290);
		g_addU32 = reinterpret_cast<WriteValueFn>(clientBase + 0xF88A0);
		g_sendPacket = reinterpret_cast<WriteValueFn>(clientBase + 0xF8E40);
		g_drawItem = reinterpret_cast<DrawItemFn>(clientBase + 0xB5990);
		g_networkBufferAddress = clientBase + 0x3998AC;
		g_playerIdAddress = clientBase + 0x23FE98;
		g_clientBaseAddress = clientBase;
		return true;
	}

	void Shutdown() noexcept
	{
		DetachInputWindow();
		std::lock_guard<std::mutex> lock(g_mutex);
		g_state = StoreState();
		g_visible.store(false, std::memory_order_release);
		g_beginPacket = nullptr;
		g_addU32 = nullptr;
		g_sendPacket = nullptr;
		g_drawItem = nullptr;
		g_networkBufferAddress = 0;
		g_playerIdAddress = 0;
		g_clientBaseAddress = 0;
		g_sessionPlayerId = 0;
	}

	bool Toggle() noexcept
	{
		if(!should_use_store)
			return false;

		bool closing = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if(g_state.visible)
			{
				SetVisibleLocked(false);
				g_state.pressedButton = BUTTON_NONE;
				g_state.draggedScrollbar = BUTTON_NONE;
				g_state.scrollbarGrabOffset = 0;
				closing = true;
			}
			else if(!IsOnline())
				return false;
			else
			{
				SetVisibleLocked(true);
				g_state.loading = !g_state.hasData;
				g_state.historyMode = false;
				g_state.status = g_state.hasData ? "" :
					"Waiting for server Store data...";
				g_state.statusError = false;
				g_state.draggedScrollbar = BUTTON_NONE;
				g_state.scrollbarGrabOffset = 0;
			}
		}
		if(closing)
		{
			ReleaseCapture();
			return true;
		}
		EnsureInputWindow();
		return true;
	}

	void Close() noexcept
	{
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			SetVisibleLocked(false);
			g_state.loading = false;
			g_state.pressedButton = BUTTON_NONE;
			g_state.draggedScrollbar = BUTTON_NONE;
			g_state.scrollbarGrabOffset = 0;
		}
		ReleaseCapture();
	}

	bool IsVisible() noexcept
	{
		return g_visible.load(std::memory_order_acquire);
	}

	bool HandleMouseMessage(WPARAM message,
		const MSLLHOOKSTRUCT& mouse) noexcept
	{
		if(!IsVisible())
			return false;
		// Normal client-window messages use the same logical coordinates as
		// the renderer. Keep the low-level hook only as a fallback.
		if(g_originalWndProc)
			return false;
		HWND window = ClientWindow();
		if(!window)
			return false;

		POINT point = mouse.pt;
		if(!ScreenToClient(window, &point))
			return true;
		const short wheelDelta = message == WM_MOUSEWHEEL ?
			static_cast<short>(HIWORD(mouse.mouseData)) : 0;
		return HandleClientMouseMessage(message, point.x, point.y, wheelDelta);
	}

	void HandleServerPacket() noexcept
	{
		NativePacket packet;
		if(!NativePacketBuffer::Capture(g_networkBufferAddress,
			NativePacketBuffer::MAXIMUM_SIZE, packet))
		{
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Could not capture the native packet buffer"));
			return;
		}

		DWORD playerId = 0;
		if(!ReadPlayerId(playerId))
		{
			DiscardNativePacketRemainder(packet);
			StoreLog("Discarded Store packet outside an authenticated session");
			return;
		}

		std::vector<BYTE> payload;
		try {
			payload.resize(packet.size - packet.start);
		} catch(const std::bad_alloc&) {
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Not enough memory for the Store packet"));
			return;
		} catch(const std::exception& exception) {
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(exception.what());
			return;
		}
		if(payload.empty() ||
			!CopyNativePacketRemainder(packet, payload.data(), payload.size()))
		{
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Could not copy the native Store packet"));
			return;
		}

		ParsedResponse response;
		std::string error;
		std::size_t consumed = 0;
		try {
			PacketReader reader(payload.data(), payload.size());
			if(!ParseResponse(reader, response, error))
			{
				DiscardNativePacketRemainder(packet);
				SetMalformedPacketError(error);
				return;
			}
			consumed = reader.Position();
			if(response.catalog)
				response.catalog->fingerprint = Fingerprint(payload.data(), consumed);
		} catch(const std::bad_alloc&) {
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Not enough memory while parsing Store data"));
			return;
		} catch(const std::exception& exception) {
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(exception.what());
			return;
		}

		if(!NativePacketBuffer::SetPosition(g_networkBufferAddress, packet,
			consumed))
		{
			DiscardNativePacketRemainder(packet);
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Native packet position changed during parsing"));
			return;
		}

		try {
			ApplyParsedResponse(std::move(response), playerId);
		} catch(const std::bad_alloc&) {
			SetMalformedPacketError(TIBIA_DIAGNOSTIC_TEXT(
				"Not enough memory while applying Store data"));
		} catch(const std::exception& exception) {
			SetMalformedPacketError(exception.what());
		}
	}

	void Draw(int surface, bool online) noexcept
	{
		if(!online)
		{
			bool releaseCapture = false;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				releaseCapture = g_state.visible;
				if(g_sessionPlayerId != 0 || g_state.visible ||
					g_state.hasData || g_state.loading)
				{
					ResetSessionLocked(0);
				}
			}
			if(releaseCapture)
				ReleaseCapture();
			return;
		}
		DWORD playerId = 0;
		if(!ReadPlayerId(playerId))
			return;
		StoreState snapshot;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if(g_sessionPlayerId != playerId)
				ResetSessionLocked(playerId);
			if(!g_state.visible)
				return;
			snapshot = g_state;
		}
		HWND window = ClientWindow();
		if(!window)
			return;
		const Layout layout = BuildLayout(window);
		DrawStore(surface, layout, snapshot);
	}
}
