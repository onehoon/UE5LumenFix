#pragma once

// UE5LumenFix performs no runtime logging; preserve call sites as no-ops.
#define DXGI_P_LOG_SYSTEM(NAME)
#define DXGI_P_LOG(MSG) do { } while (false)
#define DXGI_P_LOGF(...) do { } while (false)
