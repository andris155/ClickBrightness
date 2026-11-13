# ClickBrightness Language Translations

This directory contains JSON language files for ClickBrightness.

## Adding a New Language

1. Copy `en-US.json` and rename it to your target language locale code, for example:
   - `de-DE.json` (German)
   - `es-ES.json` (Spanish)
   - `fr-FR.json` (French)
   - `hu-HU.json` (Hungarian)
   - `ja-JP.json` (Japanese)
   - `zh-CN.json` (Simplified Chinese)

2. Update the language header attributes:
   ```json
   {
     "__language_code": "de-DE",
     "__language_name": "Deutsch",
     ...
   }
   ```

3. Translate the strings. Any missing keys will automatically fall back to the default English strings.

4. Place the `.json` file in the `lang\` folder next to `ClickBrightness.exe`. ClickBrightness will automatically detect and list the new language in the Settings menu without requiring any code changes or recompilation!
