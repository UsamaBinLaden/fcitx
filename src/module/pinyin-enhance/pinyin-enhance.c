/***************************************************************************
 *   Copyright (C) 2011~2012 by CSSlayer                                   *
 *   wengxt@gmail.com                                                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.              *
 ***************************************************************************/

#include <errno.h>
#include <iconv.h>
#include <unistd.h>
#include <ctype.h>

#include <libintl.h>
#include "pinyin-enhance.h"
#include "pinyin-enhance-spell.h"
#include "pinyin-enhance-cfp.h"
#include "pinyin-enhance-sym.h"

#define LOGLEVEL DEBUG

static void *PinyinEnhanceCreate(FcitxInstance *instance);
static void PinyinEnhanceDestroy(void *arg);
static void PinyinEnhanceReloadConfig(void *arg);
static void PinyinEnhanceAddCandidateWord(void *arg);
static void PinyinEnhanceResetHook(void *arg);
static boolean PinyinEnhancePostInput(void *arg, FcitxKeySym sym,
                                      unsigned int state,
                                      INPUT_RETURN_VALUE *retval);
static boolean PinyinEnhancePreInput(void *arg, FcitxKeySym sym,
                                     unsigned int state,
                                     INPUT_RETURN_VALUE *retval);

CONFIG_BINDING_BEGIN(PinyinEnhanceConfig)
CONFIG_BINDING_REGISTER("Pinyin Enhance", "ShortAsEnglish", short_as_english);
CONFIG_BINDING_REGISTER("Pinyin Enhance", "AllowReplaceFirst",
                        allow_replace_first);
CONFIG_BINDING_REGISTER("Pinyin Enhance", "DisableSpell", disable_spell);
CONFIG_BINDING_REGISTER("Pinyin Enhance", "MaximumHintLength", max_hint_length);
CONFIG_BINDING_REGISTER("Pinyin Enhance", "InputCharFromPhraseString",
                        char_from_phrase_str);
CONFIG_BINDING_REGISTER("Pinyin Enhance", "InputCharFromPhraseKey",
                        char_from_phrase_key);
CONFIG_BINDING_END()

CONFIG_DEFINE_LOAD_AND_SAVE(PinyinEnhance, PinyinEnhanceConfig,
                            "fcitx-pinyin-enhance")

FCITX_DEFINE_PLUGIN(fcitx_pinyin_enhance, module, FcitxModule) = {
    .Create = PinyinEnhanceCreate,
    .Destroy = PinyinEnhanceDestroy,
    .SetFD = NULL,
    .ProcessEvent = NULL,
    .ReloadConfig = PinyinEnhanceReloadConfig
};

static int
check_im_type(PinyinEnhance *pyenhance)
{
    FcitxIM *im = FcitxInstanceGetCurrentIM(pyenhance->owner);
    if (!im)
        return PY_IM_INVALID;
    if (strcmp(im->uniqueName, "pinyin") == 0 ||
        strcmp(im->uniqueName, "pinyin-libpinyin") == 0 ||
        strcmp(im->uniqueName, "googlepinyin") == 0 ||
        strcmp(im->uniqueName, "shuangpin-libpinyin") == 0)
        return PY_IM_PINYIN;
    if (strcmp(im->uniqueName, "shuangpin") == 0)
        return PY_IM_SHUANGPIN;
    if (strcmp(im->uniqueName, "sunpinyin") == 0) {
        FcitxModuleFunctionArg arg;
        boolean sp = false;
        char *str;
        arg.args[0] = "";
        arg.args[1] = &sp;
        str = FcitxModuleInvokeFunctionByName(im->owner->owner,
                                              "fcitx-sunpinyin", 0, arg);
        if (str)
            free(str);
        return sp ? PY_IM_SHUANGPIN : PY_IM_PINYIN;
    }
    return PY_IM_INVALID;
}

static void*
PinyinEnhanceCreate(FcitxInstance *instance)
{
    PinyinEnhance *pyenhance = fcitx_utils_new(PinyinEnhance);
    pyenhance->owner = instance;

    if (!PinyinEnhanceLoadConfig(&pyenhance->config)) {
        free(pyenhance);
        return NULL;
    }

    PinyinEnhanceSymInit(pyenhance);

    FcitxIMEventHook event_hook = {
        .arg = pyenhance,
        .func = PinyinEnhanceAddCandidateWord,
    };
    FcitxInstanceRegisterUpdateCandidateWordHook(instance, event_hook);
    event_hook.func = PinyinEnhanceResetHook;
    FcitxInstanceRegisterResetInputHook(instance, event_hook);

    FcitxKeyFilterHook key_hook = {
        .arg = pyenhance,
        .func = PinyinEnhancePostInput
    };
    FcitxInstanceRegisterPostInputFilter(pyenhance->owner, key_hook);
    key_hook.func = PinyinEnhancePreInput;
    FcitxInstanceRegisterPreInputFilter(pyenhance->owner, key_hook);

    return pyenhance;
}

static boolean
PinyinEnhancePreInput(void *arg, FcitxKeySym sym, unsigned int state,
                      INPUT_RETURN_VALUE *retval)
{
    PinyinEnhance *pyenhance = (PinyinEnhance*)arg;
    if (!check_im_type(pyenhance))
        return false;
    if (PinyinEnhanceCharFromPhrasePre(pyenhance, sym, state, retval))
        return true;
    return false;
}

static boolean
PinyinEnhancePostInput(void *arg, FcitxKeySym sym, unsigned int state,
                       INPUT_RETURN_VALUE *retval)
{
    PinyinEnhance *pyenhance = (PinyinEnhance*)arg;
    if (!check_im_type(pyenhance))
        return false;
    if (PinyinEnhanceCharFromPhrasePost(pyenhance, sym, state, retval))
        return true;
    return false;
}

static void
PinyinEnhanceAddCandidateWord(void *arg)
{
    PinyinEnhance *pyenhance = (PinyinEnhance*)arg;
    int im_type;
    PinyinEnhanceCharFromPhraseCandidate(pyenhance);
    /* check whether the current im is pinyin */
    if (!(im_type = check_im_type(pyenhance)))
        return;
    if (PinyinEnhanceFindSym(pyenhance))
        return;
    if (!pyenhance->config.disable_spell)
        PinyinEnhanceSpellHint(pyenhance, im_type);
    return;
}

static void
PinyinEnhanceDestroy(void *arg)
{
    /* PinyinEnhance *pyenhance = (PinyinEnhance*)arg; */
}

static void
PinyinEnhanceReloadConfig(void *arg)
{
    PinyinEnhance *pyenhance = (PinyinEnhance*)arg;
    PinyinEnhanceLoadConfig(&pyenhance->config);
}

char*
PinyinEnhanceGetSelected(PinyinEnhance *pyenhance)
{
    FcitxInputState *input;
    char *string;
    input = FcitxInstanceGetInputState(pyenhance->owner);
    string = FcitxUIMessagesToCString(FcitxInputStateGetPreedit(input));
    /**
     * Haven't found a way to handle the case when the word before the current
     * one is not handled by im-engine (e.g. a invalid pinyin in sunpinyin).
     * (a possible solution is to deal with different im-engine separately).
     * Not very important though....
     **/
    *fcitx_utils_get_ascii_part(string) = '\0';
    return string;
}

static void
PinyinEnhanceResetHook(void *arg)
{
    PinyinEnhance *pyenhance = (PinyinEnhance*)arg;
    PinyinEnhanceCharFromPhraseReset(pyenhance);
}
// kate: indent-mode cstyle; space-indent on; indent-width 0;
