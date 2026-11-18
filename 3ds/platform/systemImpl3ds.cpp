// SPDX-License-Identifier: GPL-2.0-or-later
// mkxp-z-3ds: systemImpl for the Nintendo 3DS (replaces src/system/systemImpl.cpp).
// The system language comes from the console settings (CFGU); there is no user name or Wine.
#include "system.h"

#include <3ds.h>

#include <string>

std::string systemImpl::getSystemLanguage() {
	u8 lang = CFG_LANGUAGE_EN;
	if (R_SUCCEEDED(cfguInit())) {
		CFGU_GetSystemLanguage(&lang);
		cfguExit();
	}
	switch (lang) {
	case CFG_LANGUAGE_JP: return "ja_JP";
	case CFG_LANGUAGE_FR: return "fr_FR";
	case CFG_LANGUAGE_DE: return "de_DE";
	case CFG_LANGUAGE_IT: return "it_IT";
	case CFG_LANGUAGE_ES: return "es_ES";
	case CFG_LANGUAGE_ZH: return "zh_CN";
	case CFG_LANGUAGE_KO: return "ko_KR";
	case CFG_LANGUAGE_NL: return "nl_NL";
	case CFG_LANGUAGE_PT: return "pt_PT";
	case CFG_LANGUAGE_RU: return "ru_RU";
	case CFG_LANGUAGE_TW: return "zh_TW";
	default: return "en_US";
	}
}

std::string systemImpl::getUserName() { return "3DS"; }

int systemImpl::getScalingFactor() { return 1; }

bool systemImpl::isWine() { return false; }

bool systemImpl::isRosetta() { return false; }

// Only meaningful under Wine; the enum has no 3DS value.
systemImpl::WineHostType systemImpl::getRealHostType() { return WineHostType::Linux; }
