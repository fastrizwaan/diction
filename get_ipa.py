from text2ipa import get_IPA
# Convert 'hello world' to English US International Alphabet
text = 'hello world'
language = 'en'
ipa = get_IPA(text, language)
# Convert 'je parle un peu français' to IPA
text = 'je parle un peu français'
language = 'fr'
fr_ipa = get_IPA(text, language)
print(ipa)
print(fr_ipa)
