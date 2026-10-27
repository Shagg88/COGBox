// COGBox - translations (0.2.0). COGBox's text is written in English; tr() looks the English text up in the chosen
// language's file, lang\<code>.txt next to CogBox.exe, and falls back to the English when a line is missing - a
// half-done translation never breaks anything. The file: UTF-8, one "English = translation" per line, "#" comments,
// "\n" for a line break inside a text. The English text is the key, so nothing has to be numbered.
#pragma once
#include <string>
#include <vector>

struct Language { std::string code, name; };   // "fr", "Français"

// The languages COGBox offers (English first), whether or not their file is there.
const std::vector<Language> &languages();
// "auto" = Windows' display language when COGBox has it, else English; or a code from languages().
void i18n_load(const std::string &choice);
std::string i18n_current();                    // the code in use ("en" when English)
std::string i18n_auto_code();                  // what "auto" picks on this PC
// The text in the current language. A label like "Play##x" keeps its "##x" (Dear ImGui's hidden id) untouched.
const char *tr(const char *english);
std::string tr(const std::string &english);
