r"""build_allinone - the main mod "Hairwork for All - All In One": RELEASE builds of the shipped characters. Local only.

  py tools\build_allinone.py --out <folder> [--aio-version 0.1.0] [--jobs 3]     (build_release_hairwork.py calls it)

For every character in AIO_ORDER: `hairpipe.py <cfg> build --release` (Core requirement script + REQUIREMENTS.txt per mod) with
HWFA_SKELETON=game (the CR2W skeleton of every redfur is read from the user's own game bundles; tpl\fur02.redfur, the Yennefer
mod's fur, is third-party and never used) and HWFA_MOD_OUT=build\aio\<v>\pkg (the repo's mods\ folders are not touched).
Layout of the zip = the installed layout: mod<Char>Hairworks\content\... at the zip root, README.txt, REQUIREMENTS.txt.
The Vesemir / Triss scalp head textures (from <game>\mods\mod<Char>Scalp) are MERGED into that character's hair mod: its
blob0.bundle gets the scalp xbm, metadata.store is regenerated with the repo's w3lib writers over all bundles, and the scalp's
texture.cache is copied next to it (merge_scalp). No separate scalp folder.
Checks: every mod has bundle + store + requirement script, no redfur carries importFile / the third-party marker, no
third-party file names, tools\sdk_guard.py --tree on the package.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# the characters it ships; APPROVED.json first, AIO_PICKS for those that are "settled / user OK" but not listed there
# (characters.md 2026-10-06). Emhyr (user: DONE 2026-10-12) = his Groom Studio project big_c/emhyr_mcp_8 as a pipeline
# config (opt-in studio_geo key: tools/hairworks/studio_geo.py)
AIO_ORDER = ['vesemir', 'c04ma', 'avallach', 'sheala', 'dandelion', 'keira', 'lambert', 'triss', 'guillaume', 'regis', 'emhyr', 'ciri', 'zoltan', 'yennefer', 'baron', 'dettlaff', 'hjalmar', 'ves', 'dijkstra', 'radovid', 'menge', 'palmerin', 'horst', 'graden', 'annastrenger', 'ewald', 'geels', 'marlene', 'shani', 'olgierd', 'svanrige', 'priscilla', 'eveline', 'crach', 'eskel', 'margarita', 'milton', 'damien', 'philippa', 'varese', 'lugos', 'anna', 'iris', 'ermion', 'junior', 'oriana', 'tamara', 'cerys', 'donar', 'corinne', 'voorhis', 'vlodimir', 'vivienne', 'udalryk']   # yennefer back in 2026-10-14 (user: "yes in all in one") with L316 (20.15 ms GPU)
AIO_PICKS = {'avallach': 'avallach_l85b_hl70_wv_ln_hc', 'sheala': 'sheala_l89_rec_sk6_sink5only',
             'junior': 'junior_l961_tiefit',   # 2026-10-20 user 'put in All in One, the changelog will have to note (not perfect)': Studio junior_fix/junior_mcp_49 (tie at the vanilla place, ponytail separated from the hanging hair, beard hidden as in the user 20:34 save), L961 +4.0 ms vs Vesemir same launch
             'keira': 'keira_l633_mcp7',  # 2026-10-17 user 'she goes in all in one': Studio keira_nf7/keira_mcp_7 (forehead / temple roots moved onto the painted hairline); was keira_l108_user1_c09
             'lambert': 'lambert_l76_ez10_vol5d_wr15_b07_f8',
             'emhyr': 'emhyr_l255_mcp8',
             'ciri': 'ciri_l259_cr10rib',
             'yennefer': 'yennefer_l1295_oldphys',  # 2026-11-04 user "Replace anyway" (L1295: shared old-physics settings re-created on l1275_alt; right side lower lengths greyer, FPS not measured). Was yennefer_l1275_alt: 2026-11-01 user (Nexus report, alt look on cards): "you only need to transfer the same hairwork" -> yennefer_l1119_s80 + hair_alias_templates c_03 (no_pendant / naked / lingerie / DLC gown get the same fur; c_01 fur byte-identical to 0.4.6); in-game L1283 single AIO, user "Yes, all HairWorks". Was yennefer_l1119_s80: 2026-10-24 user: Yennefer updated (ink black + face/neck colliders + stiffness, L1101-L1119); was l365_user: 2026-10-15 user "for release and game run": Studio yen_split/yennefer_user_2025 (split groom + user edits); was yennefer_l330_user. 2026-10-15 user "my latest Yenn save: go for release" (vesgen/yennefer_user_1112, density 1.3->1.11 for budget, perf accepted by user). Was yennefer_l316_mcp35s, 2026-10-14: Studio yen6/yennefer_mcp_35 (top front fix) with the user's stiffness floor (0.717 / root 0.366 / tip 0), L316 20.15 ms GPU
             'zoltan': 'zoltan_l808_user2043',   # 2026-10-18 user 'Ship zoltan_l808_user2043': full beard (zolfix lip85 geometry) + user's Studio look edit (zoltan_user_2043), density_flat .85; L808 GPU 15.58 vs Vesemir 16.44 ms. Was zoltan_l302_perf
             'baron': 'baron_l1118_nocards',   # 2026-10-24 user "REMOVE the cardboard, NO HYBRID": l323_user beard + Studio eyebrow guides (baron2/baron_mcp_3), no _brows card mesh
             'dettlaff': 'dettlaff_l352_user',   # 2026-10-15 user: Studio edit of dettlaff_l350_opt (k10 + Coarse short, width .675) for release; was dettlaff_l326_user
             'hjalmar': 'hjalmar_l327_user', 'ves': 'ves_l331_user',
             # 2026-10-15 user "i edited: radovid, menge, djikstra; rest is good, all 5 ready for release and next game run": Studio beard-only look / physics edits (studio_geo_beard) for Radovid, Menge, Dijkstra (beard_<char>/<char>_user_1302); Palmerin, Horst as built
             'dijkstra': 'dijkstra_l336_user', 'radovid': 'radovid_l336_user', 'menge': 'menge_l336_user',
             'palmerin': 'palmerin_l332_beard', 'horst': 'horst_l334_beard',
             'graden': 'graden_l357_user',
             'annastrenger': 'annastrenger_l361_user',
             'ewald': 'ewald_l367_user',
             'geels': 'geels_l368_user',
             'olgierd': 'olgierd_l628_redbeard',   # 2026-10-18 user 'the latest Olgierd von Everec should be in the All In One': user 0850 save, beard colour = hair colour (installed modOlgierdHairworks)
             'margarita': 'margarita_l891_s85',   # 2026-10-19 user 'put stiffness strength at 0.85 and call it a day, put her in all in one': L885 fine (Studio marg2/margarita_marg2_c20s, PT Fine optimised, 2385 guides, +2.45 ms GPU vs Vesemir at L885) + stiffnessStrength 0.85
             'crach': 'crach_l816_user0602',   # 2026-10-19 user 'its good put him in the All in one release': his Studio edit (crach9/crach_user_0602 = crach_mcp_17 + stiffness .761 / root 1 / tip 0) of the L814 pick (colliders + Locks cut 831 guides, 8 CVs, Coarse short beard); m17 L814 GPU 15.04 vs Vesemir 16.31 ms
             'eveline': 'eveline_l783_lookg',   # 2026-10-18 user 'remove that bulge of the left ear and then she is good for a game capture then All in one': Studio eve7/eveline_look_g (colliders + Locks deep cut, longer, even nape, tapered fringe / sideburn, left-ear bulge hugged), L783
             'priscilla': 'priscilla_l797_mcp14',   # 2026-10-18 user 'ok she seems ready for all in one' (mcp_11), then back colliders at the coat surface + shoulder-blade hair lifted off the vest (user: hairs clip into the middle of the back): Studio prisc2/priscilla_mcp_14, L797 20.36 ms vs Vesemir 21.87; was (Locks deep cut, front lock, both-side split fills, PT Straight sleek optimised, dlw .5), L773; GPU ~ Vesemir (mcp_10 13.87 vs 13.76 ms, L764)
             'svanrige': 'svanrige_l684_pick',   # 2026-10-18 user 'the latest optimized Svanrige can also go in': Locks deep cut (Studio r10, 8 CVs, dlw .8), GPU ~ Vesemir (L682 / L684)
             'marlene': 'marlene_l561_mcp17',
             'milton': 'milton_l904_d280tb',
             'damien': 'damien_l899_user1908',
             'varese': 'varese_l932_final',
             'lugos': 'lugos_l951_user1535',
             'ermion': 'ermion_l954_greydark',  # 2026-10-20 user 'put ermion_l954_greydark in the All In One as Ermion (beard)': beard-only Studio beard_ermion/ermion_mcp_14 (user save 19:24 + head/jaw/neck weights, stiffer sim, dark grey); L954
             'anna': 'anna_l952_user1540',
             'iris': 'iris_l955_user1556',   # 2026-10-20 user 'i edited her, export and push in all in one': her Studio save 15:56 over iris2_d25 (iris2/iris2_user_1556): side hair swept over the ears into a rolled border, nape roll under the rose, sides layered inside the vanilla outline, strands only (split_cards null); L955 GPU 25.11 vs Vesemir 22.37 same launch (short dx12user.settings); L957 re-shoot with the full settings (PT hair blocks): same look, 26.25 vs 24.06   # 2026-10-20 user 'i edited a bit, export her and she goes in All in One': her Studio save 15:40 over anna_mcp_8k16n (anna1/anna_user_1540): L950 neck colliders + side masses out of the neck + clump sway, K16 strands; L952 GPU 25.43 vs Vesemir 22.40 same launch   # 2026-10-20 user 'he looks good now, put in AIO': his Studio save 15:35 over lugos_mcp_17 (beard_lugos/lugos_user_1535), beard-only Coarse short, L951 GPU 20.8 ms (this run's Vesemir reading was an outlier)   # 2026-10-20 user 'put both stiffsness to 0.8, enable physics on beard and call ita day, put in AIO': long black beard (Studio beard_varese/varese_user_1850) + mohawk, both simulated, stiffness .8; GPU not measured for this exact config (L905 both simulated +15 ms vs Vesemir)   # 2026-10-19 user 'its fine, release as such': his Studio save of damien_mcp_6 (beard_damien/damien_user_1908, PT Soft wave optimised), L899 GPU 12.44 vs Vesemir 17.19 same launch; near-black dense moustache accepted   # 2026-10-19 user 'its fine, release as this': L904 d280tb (Studio milton_mcp_4 + hairs_cm2 280 + studio_defaults l592), GPU 17.59 vs Vesemir 27.73 same launch; bare cheek / under-lip patches accepted   # 2026-10-16 user mcp14 approved + 'a look change on that area is OK': left-temple root stubs fixed (folded growth faces re-triangulated, right low-temple faces removed; guides / colliders / look = mcp14), L561   # 2026-10-16 user "fix only Marlene's temple tufts ... then add her to the All In One if clean" + "fix the flare": her 18:11 Studio save (marlene_user_1811, L489) with the temple tufts re-laid and the ear spheres refit (marlene_t1/marlene_mcp_14); prepared, not built   # 2026-10-15 user "ready for release and gpu run": Studio hair_geels/geels_user_2116   # 2026-10-15 user "put him for release and next game run": Studio hair_ewald/ewald_user_2101, L367 18.9 ms GPU   # 2026-10-15 user "she is candidate for release and GPU run": Studio hair_annastrenger/annastrenger_user_2010   # 2026-10-15 user "ready for release and game run": Studio hair_graden5/graden_user_1919 (--allow-perf, L357 25.2 ms GPU)   # 2026-10-15 user "go for my last Ves edit": Studio vesgen/ves_round_37 user save (ves_user_1126_r37), GPU cost 239% accepted by the user   # 2026-10-15 user "hjal is good, ready for release": hair+beard Studio edit, hair colour = beard   # 2026-10-15 user "install my last dettlaff edit for the next release": Studio dett8/dettlaff_user_0905, exported --allow-perf (user: "I want my dettlaff edit")   # 2026-10-15 user "my barron edit btw > good for release in all in one": Studio baron1/baron_user_0646 beard-only (studio_geo_beard, Coarse short)
             'oriana': 'oriana_l968_user',
             'tamara': 'tamara_l1008_user',
             'cerys': 'cerys_l909_m13',
             'donar': 'donar_l1032_t',
             'voorhis': 'voorhis_l1270_mcp37',   # 2026-10-31 user 'Build All In One 0.4.6 with Vivienne, Udalryk and Voorhis L1250': L1270 = L1250 (mcp_27, ponytail separated from the back) + the stray fixes up to mcp_37 (no fan beside the tail, no veil in the gap under it); was voorhis_l1142_mcp19 in 0.4.5
             'vlodimir': 'vlodimir_l1127_face',   # 2026-10-27 user 'Put ... Vlodimir (L1127 build) ... in the All In One': Studio vlodimir_mcp_17 (PT Fine optimised, face collider shell so the loose strand stays off the face), L1127
             'vivienne': 'vivienne_l1244_p23',   # 2026-10-30 user 'she looks ok in game? if so we can stop and put her in all in one': Studio vivienne2/vivienne_mcp_23 (front scalp filled, temple flare clamped, forehead strands removed: off-scalp front roots, parting-tip stubs, forehead-hanging guides); L1244, 6 sides
             'udalryk': 'udalryk_l1235_h64b',   # 2026-10-30 user 'yes he seems ok, go for all in one': Studio udal1/udalryk_mcp_64 (temple corners at vanilla's visible hairline, root taper .12) + PT Tapered tips beard (mcp_62, colour 80/40/27) + look.card_tint.bleed (no dark dashes along the edge); L1235, 6 sides
             'corinne': 'corinne_l1057_m38la'}   # 2026-10-27 user 'Corinne seem ok, go in All in one': Studio corinne_k1/corinne_mcp_38 (PT Fine optimised, hidden back guides thinned 2114 -> 1770, thin parting-start layers, wind root .2, lower-back colliders); L1057 GPU +2.8 ms vs Vesemir same launch
               # 2026-10-21 user 'add him to all in one': user Studio save donar_user_1241 + mustache re-comb (donar7/donar_look_t2) + studio_geo_beard.bind_head_box (mustache follows the head); braids = vanilla cards tinted, beard colour_tint texture.cache; L1031 GPU 24.42 vs Vesemir 22.33   # 2026-10-21 user 'lets keep her as she is and put in all in one': Studio cerys2/cerys_mcp_13, PT Fine optimised material, no pins / soft braid (braid tests L973-L1018 did not move the braid as one piece); L909 GPU +1.75 ms vs Vesemir same launch; crown parting scalp patch accepted   # 2026-10-21 user 'i edited her a bit, put her in all in one': her Studio save 07:16 over tamara6/tamara_mcp_13 (snapshot tamara_user_0716); L1008 release check   # 2026-10-20 user 'ok she goes in all in one': her Studio save 16:46 (projects/oriana_user_1647) over L960 skf (side ropes over the band ends, skin-fit synced back rope, back-left fill), PT Soft wave optimised, full stiffness, under_band clamp az_only/reach 12/end_trim 4; L968 GPU 24.54 vs Vesemir 22.25 (drew as a flat mass in game that day, as every Oriana build since L937)
# path-tracing picks (user OK 2026-10-13, measured with fpsrun): these win over APPROVED.json, which stays frozen
AIO_PT_PICKS = {'eskel': 'eskel_l871_c1',   # 2026-10-19 user 'put eskel_l871_c1 in the All In One instead of s16': user Studio edit of u2 (1305 guides, stiffness .9 / tip .35) + collar tuck (back hair inside the coat collar); L871 GPU 21.84 vs Vesemir 17.14
                'lambert': 'lambert_l285_me3', 'guillaume': 'guillaume_l275_p09r15c10', 'triss': 'triss_l275_c07',
                'regis': 'regis_l323_user',
                'shani': 'shani_l627_eyefix',
                'vesemir': 'vesemir_l1094_rigid',   # 2026-10-24 user: Vesemir updated (cutscene hold, L1086-L1094), tip and bend .5
                'philippa': 'philippa_l887_u1646'}  # 2026-10-20 user 'Philippa lets ignore the braid movement and put her in All in One': her Studio save philippa_user_1646 (phil3 c10 + full stiffness, PT Soft wave optimised; right side plait tucked under the blindfold, head spheres 2 mm inside it); L887 GPU 15.69 vs Vesemir 17.43 ms
# 2026-10-17 user 'Shani with eye fix can go in all in one': her Studio save shani_user_1427 minus 2 guides whose interpolated hair crossed her left eye (shani_eye1/shani_mcp_17); not shot in game yet (L627 pending), GPU not measured. Regis: 2026-10-15: user's own Studio edit of Regis (regis2/regis_user_0641) + Coarse short HairWorks beard, replaces regis_v14 ("put that in next release"); here because APPROVED.json wins over AIO_PICKS
# folder name overrides: Triss loads before the lore-friendly Triss mod (modtriss_lorehair), which otherwise overrides her hair
AIO_MOD_NAMES = {'triss': 'mod000_TrissHairworks'}
# optional folders (zip folder optional\<mod>, so "copy everything" does not install them): built with --release --mod-guard
# --mod <mod>. Headband Ciri (user: add to the AIO 2026-10-14) converts bububull's "Ciri - long wavy hair" (Nexus 7063) and
# needs it at runtime (mod_guard.py); it patches the same ciri.w2ent as modCiriHairworks, so users install one of the two
AIO_OPTIONAL = {'ciri_headband': ('ciri_l1124_w8c3', 'mod000_CiriWavyHairworks')}   # 2026-10-27 user 'Put ... the Ciri headband parting fix (ciri_l1124_w8c3) in the All In One': top-hair parting guides + thicker roots, less clumping (no parting holes from above, L1124); was ciri_l1090_l40c. 2026-10-25 user 'swap the Ciri headband optional to the L1090 lift40c build': crown floor + edge split (L1085), softer physics, simulated roots 40 mm up the strand + root colour = tip colour (no dark strip under the band); was ciri_l964_rnd10c6 (2026-10-20), ciri_l313_guard
OPTIONAL_DIR = 'optional'
AIO_EXTRA_MODS = {'vesemir': 'modVesemirScalp', 'triss': 'modTrissScalp'}
AIO_NAMES = {'vesemir': 'Vesemir', 'c04ma': 'generic short-hair men (c04ma, 79 NPCs)', 'avallach': "Avallac'h", 'sheala': 'Sheala',
             'dandelion': 'Dandelion', 'keira': 'Keira Metz', 'lambert': 'Lambert', 'triss': 'Triss (needs the lore-friendly Triss mod)',
             'guillaume': 'Guillaume', 'regis': 'Regis', 'emhyr': 'Emhyr var Emreis', 'ciri': 'Ciri',
             'yennefer': 'Yennefer (travel outfit + alternative look)', 'zoltan': 'Zoltan Chivay', 'baron': 'Bloody Baron (beard)', 'dettlaff': 'Dettlaff', 'hjalmar': 'Hjalmar', 'ves': 'Ves',
             'dijkstra': 'Sigismund Dijkstra (beard)', 'radovid': 'Radovid V (beard)', 'menge': 'Caleb Menge (beard)',
             'palmerin': 'Palmerin de Launfal (beard)', 'horst': 'Horst Borsodi (beard)', 'graden': 'Graden', 'annastrenger': 'Anna Strenger', 'ewald': 'Ewald', 'geels': "Ge'els", 'marlene': 'Marlene', 'shani': 'Shani', 'olgierd': 'Olgierd von Everec', 'svanrige': 'Svanrige', 'eveline': 'Eveline Gallo', 'priscilla': 'Priscilla', 'crach': 'Crach an Craite', 'margarita': 'Margarita Laux-Antille', 'eskel': 'Eskel',
             'milton': 'Milton de Peyrac-Peyran (beard)',
             'damien': 'Damien de la Tour (beard)',
             'philippa': 'Philippa Eilhart',
             'varese': 'Carlo Varese (beard)',
             'lugos': 'Lugos the Mad (beard)',
             'anna': 'Anna Henrietta',
             'oriana': 'Oriana', 'tamara': 'Tamara Strenger', 'cerys': 'Cerys an Craite', 'donar': 'Donar an Hindar', 'corinne': 'Corinne Tilly', 'voorhis': 'Morvran Voorhis', 'vlodimir': 'Vlodimir von Everec', 'vivienne': 'Vivienne de Tabris', 'udalryk': 'Udalryk', 'junior': 'Whoreson Junior',
             'ermion': 'Ermion (beard)',
             'iris': 'Iris von Everec',
             'ciri_headband': 'Ciri with headband (optional folder, needs "Ciri - long wavy hair" by bububull)'}

README = """Hairwork for All - All In One %(v)s  (Nexus witcher3 mod 13996; patch 5.0 next-gen, DX12)
by WinterElfeas

NVIDIA HairWorks strands instead of hair cards for:
%(chars)s

REQUIRED (the game will not start without the first one: every mod here carries a script that checks for it)
  1. Hairwork for All - Core, v%(core)s or newer: the file "Hairwork for All - Core" of this same Nexus page
     (https://www.nexusmods.com/witcher3/mods/13996). Copy mod0000HairworkForAllCore into <game>\\mods.
  2. Graphics Control (https://www.nexusmods.com/witcher3/mods/13349): PT hair > "Hair root transparency mode" = Mode 0
     (path-traced hair shows see-through gaps in mode 1).
  3. HairWorks enabled in the game's graphics options.

INSTALL
  Copy the mod folders you want from this zip into <game>\\mods (Core first). Remove a folder to get that character's vanilla
  hair back. One folder per character: modVesemirHairworks, modC04maHairworks, ... The Vesemir and mod000_TrissHairworks folders also hold
  the recoloured scalp texture under the hair (the game's own head texture, recoloured, and nothing else).

TRISS
  Triss's hair is built on the "lore-friendly long hair for Triss" mod (Nexus 6457) by its author. Install that mod yourself:
  it is NOT included here, and this one needs it. A small script in her folder checks for it while the game runs: without
  that mod, Triss keeps her vanilla hair (no HairWorks) and the game shows a message that asks for it.
  Her folder is named mod000_TrissHairworks on purpose: the name sorts before modtriss_lorehair, so this mod loads first and
  the lore-friendly hair mod does not override her HairWorks hair. Do not rename it.
  If you use a mods.settings priority list (Documents\\The Witcher 3\\mods.settings, written by Script Merger or Vortex),
  give mod000_TrissHairworks a higher priority than modtriss_lorehair there: that list overrides the alphabetical order.

CIRI WITH HEADBAND (optional)
  The folder optional\\mod000_CiriWavyHairworks is Ciri's long wavy hair with headband from the mod "Ciri - long wavy hair"
  by bububull (Nexus 7063), converted to HairWorks. Install that mod yourself: it is NOT included here, and this folder needs
  it. A small script in the folder checks for it while the game runs: without it, Ciri keeps her vanilla hair and the game
  shows a message that asks for it.
  Install ONE Ciri folder: modCiriHairworks (her vanilla haircut) OR mod000_CiriWavyHairworks (headband). Both change the
  same Ciri template; with both installed, the headband one wins. Copy the mod000_CiriWavyHairworks folder itself into
  <game>\\mods, not the optional folder.

Mods that replace the same hair templates of these characters conflict (the later mod folder wins).
Made with Hairwork Studio (same Nexus page). Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation.
No third-party mod files are included.
"""


def configs():
    """-> [(char, cfg, mod)] in AIO_ORDER."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'hairworks'))
    import approved
    dm = approved.default_map()
    out = []
    for ch in AIO_ORDER:
        cfg = AIO_PT_PICKS.get(ch) or dm.get(ch) or AIO_PICKS.get(ch)
        if not cfg:
            sys.exit('no config for ' + ch)
        if ch in AIO_PICKS and ch not in AIO_PT_PICKS and dm.get(ch) not in (None, cfg):
            print('note: APPROVED.json now has %s for %s (AIO_PICKS: %s): using APPROVED' % (dm[ch], ch, AIO_PICKS[ch]))
        with open(os.path.join(ROOT, 'tools', 'hairworks', 'chars', cfg + '.json'), encoding='utf-8-sig') as fh:
            C = json.load(fh)
        out.append((ch, cfg, AIO_MOD_NAMES.get(ch, C['mod'])))
    return out


def merge_scalp(hair_dir, scalp_dir):
    """Merge a scalp mod (blob0.bundle + texture.cache) into a hair mod: same files, same bytes per entry. The bundles and
    metadata.store are rewritten with w3lib's writers (no hand-swapped metadata.store); texture.cache is copied."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import w3lib
    hc, sc = os.path.join(hair_dir, 'content'), os.path.join(scalp_dir, 'content')
    groups = []
    for bn in ('blob0.bundle', 'buffers0.bundle'):
        hp = os.path.join(hc, bn)
        groups.append([(e.name, w3lib.bundle_read(hp, e)) for e in w3lib.bundle_entries(hp)] if os.path.isfile(hp) else [])
    sp = os.path.join(sc, 'blob0.bundle')
    extra = [(e.name, w3lib.bundle_read(sp, e)) for e in w3lib.bundle_entries(sp)]
    have = {n.lower() for g in groups for n, _ in g}
    dup = [n for n, _ in extra if n.lower() in have]
    if dup:
        sys.exit('scalp merge: file names in both mods: %s' % dup)
    if os.path.isfile(os.path.join(hc, 'texture.cache')) or not os.path.isfile(os.path.join(sc, 'texture.cache')):
        sys.exit('scalp merge: unexpected texture.cache state in %s / %s' % (hair_dir, scalp_dir))
    groups[0] += extra
    bundles = [w3lib.write_legacy_bundle(os.path.join(hc, 'blob0.bundle'), groups[0])]
    if groups[1]:
        bundles.append(w3lib.write_legacy_bundle(os.path.join(hc, 'buffers0.bundle'), groups[1]))
    w3lib.write_legacy_metadata_store(os.path.join(hc, 'metadata.store'), bundles)
    shutil.copyfile(os.path.join(sc, 'texture.cache'), os.path.join(hc, 'texture.cache'))
    for bn, g in zip(('blob0.bundle', 'buffers0.bundle'), groups):
        bp = os.path.join(hc, bn)
        if g:
            got = {e.name: w3lib.bundle_read(bp, e) for e in w3lib.bundle_entries(bp)}
            assert got == dict(g), 'scalp merge: %s differs after rewrite' % bn
    # the scalp's entry must be byte-identical to its source
    got = {e.name: w3lib.bundle_read(os.path.join(hc, 'blob0.bundle'), e) for e in w3lib.bundle_entries(os.path.join(hc, 'blob0.bundle'))}
    for n, d in extra:
        assert got[n] == d
    return [n for n, _ in extra]


# ---------------------------------------------------------------------------------- single-folder All In One (opt-in)
# 2026-10-21 user: "witcher has a 200 mods limits ... create a REAL All In One single file for Hairwork, and keep the current
# one as separate main file called All In Zip (Split)". --single merges the split package (pkg\, untouched: the split zip
# stays byte-identical) into ONE mod folder: every bundle entry as stored in the split build (one blob0 + one buffers0
# bundle, metadata.store regenerated over both with w3lib's writers), the scalps' texture.cache entries merged into one
# cache (pixel blobs copied as stored), the per-mod guard scripts copied and ONE Core requirement script instead of 46.
# mod000_ sorts before modtriss_lorehair (Triss) and after mod000_CiriWavyHairworks (the optional headband Ciri still wins).
SINGLE_MOD = 'mod000_HairworkForAll'
SINGLE_ZIP = 'HairworkForAll_AllInOneSingle_%s.zip'


def merge_texture_caches(out_path, paths):
    """v6 texture.cache holding every entry of `paths` (v6 caches, with or without mip tables): each entry's stored blob is
    copied as it is, its page and mip-table index re-based. -> entry names"""
    import struct
    import texcache as TC
    blob, strings, ents, mtab, names = bytearray(), bytearray(), bytearray(), [], []
    for p in paths:
        c = TC.TextureCache(p)
        assert c.version == 6, '%s: texture.cache v%d' % (p, c.version)
        for e in c.entries:
            if e.name.lower() in {n.lower() for n in names}:
                sys.exit('texture.cache merge: %s in two caches' % e.name)
            with open(p, 'rb') as fh:
                fh.seek(e.page * 4096)
                data = fh.read(e.zsize)
            mi, mn = struct.unpack_from('<ii', e.raw, 32)
            raw = bytearray(e.raw)
            struct.pack_into('<IiI', raw, 0, e.hash, len(strings), len(blob) // 4096)
            if mn:
                struct.pack_into('<i', raw, 32, len(mtab))
                mtab += c.mips[mi:mi + mn]
            blob += data + bytes(-len(data) % 4096)
            strings += e.name.encode('latin1') + b'\0'
            ents += raw
            names.append(e.name)
    with open(out_path, 'wb') as f:
        f.write(blob)
        f.write(struct.pack('<%dI' % len(mtab), *mtab))
        f.write(strings)
        f.write(ents)
        f.write(struct.pack('<QIIII4sI', 0, len(blob) // 4096, len(names), len(strings), len(mtab), b'HCXT', 6))
    # re-read: every entry's blob and mip table as in its source
    m = TC.TextureCache(out_path)
    for p in paths:
        c = TC.TextureCache(p)
        for e in c.entries:
            g = m.by_name[e.name.lower()]
            with open(p, 'rb') as a, open(out_path, 'rb') as b:
                a.seek(e.page * 4096)
                b.seek(g.page * 4096)
                assert a.read(e.zsize) == b.read(g.zsize)
            assert e.raw[12:32] == g.raw[12:32] and e.raw[36:] == g.raw[36:]
            (mi, mn), (gi, gn) = struct.unpack_from('<ii', e.raw, 32), struct.unpack_from('<ii', g.raw, 32)
            assert c.mips[mi:mi + mn] == m.mips[gi:gi + gn]
    return names


def split_entries(pkg, mods):
    """-> {bundle name: [(entry name, bytes)]} of the split package's mods, in mod order; an entry name met again with the
    same bytes is kept once (modJuniorHairworks' blob0 lists 5 files twice), with other bytes it stops the build."""
    import w3lib
    out, seen = {'blob0.bundle': [], 'buffers0.bundle': []}, {}
    for mod in mods:
        c = os.path.join(pkg, mod, 'content')
        bns = sorted(f for f in os.listdir(c) if f.endswith('.bundle'))
        if set(bns) - set(out):
            sys.exit('single All In One: %s has bundles %s' % (mod, bns))
        for bn in bns:
            bp = os.path.join(c, bn)
            for e in w3lib.bundle_entries(bp):
                d = w3lib.bundle_read(bp, e)
                k = e.name.lower()
                if k in seen:
                    if seen[k][1] != d:
                        sys.exit('single All In One: %s in %s and %s with different bytes' % (e.name, seen[k][0], mod))
                    continue
                seen[k] = (mod, d)
                out[bn].append((e.name, d))
    return out


def build_single(pkg, out, aiover, core_version, det_zip, scan, cfgs):
    """pkg = the split package (build\\aio\\<v>\\pkg). -> single zip path. pkg is only read."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    sys.path.insert(0, os.path.join(ROOT, 'core'))
    import w3lib
    import hwcore as K
    mods = [m for _ch, _c, m in cfgs]
    spkg = os.path.join(os.path.dirname(pkg), 'single')
    if os.path.isdir(spkg):
        shutil.rmtree(spkg)
    md = os.path.join(spkg, SINGLE_MOD)
    c = os.path.join(md, 'content')
    os.makedirs(c)
    groups = split_entries(pkg, mods)
    bundles = [w3lib.write_legacy_bundle(os.path.join(c, bn), groups[bn]) for bn in ('blob0.bundle', 'buffers0.bundle')
               if groups[bn]]
    w3lib.write_legacy_metadata_store(os.path.join(c, 'metadata.store'), bundles)
    caches = [os.path.join(pkg, m, 'content', 'texture.cache') for m in mods
              if os.path.isfile(os.path.join(pkg, m, 'content', 'texture.cache'))]
    tex = merge_texture_caches(os.path.join(c, 'texture.cache'), caches) if caches else []
    # scripts: the guards (Triss lore guard, ...) as they are; the 46 requirement scripts -> one for this folder
    sd = os.path.join(c, 'scripts', 'local')
    os.makedirs(sd)
    for m in mods:
        msd = os.path.join(pkg, m, 'content', 'scripts', 'local')
        for f in sorted(os.listdir(msd)) if os.path.isdir(msd) else []:
            if f.startswith('hwfa_req_'):
                continue
            if os.path.exists(os.path.join(sd, f)):
                sys.exit('single All In One: script %s in two mods' % f)
            shutil.copyfile(os.path.join(msd, f), os.path.join(sd, f))
    for m in mods:   # nothing else may sit in a split mod's content folder
        extra = [os.path.relpath(os.path.join(dp, f), os.path.join(pkg, m)) for dp, _, fns in os.walk(os.path.join(pkg, m, 'content'))
                 for f in fns if not (f.endswith('.bundle') or f in ('metadata.store', 'texture.cache') or
                                      os.path.basename(dp) == 'local')]
        if extra:
            sys.exit('single All In One: %s has files the merge does not handle: %s' % (m, extra))
    reqs = {json.load(open(os.path.join(pkg, m, 'hairworkforall_mod.json'), encoding='utf-8'))['requires_core'] for m in mods}
    if len(reqs) != 1:
        sys.exit('single All In One: split mods require different Core versions %s' % reqs)
    K.write_release_files(md, SINGLE_MOD, reqs.pop(), dict(
        all_in_one=aiover, characters=[dict(char=ch, config=cfg, split_mod=m) for ch, cfg, m in cfgs]), salt=('allinone',))
    # verify: every entry of every split bundle is in the single bundles with the same bytes, the store lists them all
    got = {}
    for bn in ('blob0.bundle', 'buffers0.bundle'):
        bp = os.path.join(c, bn)
        if os.path.isfile(bp):
            got[bn] = {e.name: w3lib.bundle_read(bp, e) for e in w3lib.bundle_entries(bp)}
    n = 0
    for m in mods:
        for bn in ('blob0.bundle', 'buffers0.bundle'):
            bp = os.path.join(pkg, m, 'content', bn)
            for e in (w3lib.bundle_entries(bp) if os.path.isfile(bp) else []):
                assert got[bn].get(e.name) == w3lib.bundle_read(bp, e), 'single: %s of %s differs / missing' % (e.name, m)
                n += 1
    st = w3lib.read_metadata_store_v6(os.path.join(c, 'metadata.store'))
    sn = {st['strings'][fi[0]:st['strings'].index(b'\0', fi[0])].decode('latin1') for fi in st['fileinfo'][1:]}
    allnames = {nm for g in got.values() for nm in g}
    assert sn == allnames and st['end'] == st['len'], 'single: metadata.store does not list every bundle entry'
    print('  single %s: %d split entries checked, %d unique files in %d bundles, %d texture.cache entries, store OK'
          % (SINGLE_MOD, n, len(allnames), len(got), len(tex)), flush=True)
    if os.path.isdir(os.path.join(pkg, OPTIONAL_DIR)):
        shutil.copytree(os.path.join(pkg, OPTIONAL_DIR), os.path.join(spkg, OPTIONAL_DIR))
    errs = check_mod_dir(md) + scan(spkg)
    if errs:
        sys.exit('single All In One check failed:\n' + '\n'.join(errs))
    names = [ch for ch, _c, _m in cfgs]
    chars = '\n'.join('  - ' + AIO_NAMES[ch] for ch in names + list(AIO_OPTIONAL))
    with open(os.path.join(spkg, 'README.txt'), 'w', encoding='utf-8') as fh:
        fh.write(README_SINGLE % dict(v=aiover, chars=chars, core=core_version, n=len(names), mod=SINGLE_MOD))
    with open(os.path.join(spkg, 'REQUIREMENTS.txt'), 'w', encoding='utf-8') as fh:
        fh.write('Requires Hairwork for All - Core v%s or newer (Nexus 13996, file "Hairwork for All - Core") and\n'
                 'Graphics Control (Nexus 13349) with PT hair > Hair root transparency mode = Mode 0.\n'
                 'Triss also needs the lore-friendly Triss mod (Nexus 6457), not included.\n'
                 'The optional headband Ciri (optional\\mod000_CiriWavyHairworks) needs "Ciri - long wavy hair" (Nexus 7063), not included.\n'
                 'Do not install it together with the split folders (mod<Char>Hairworks) of "All In One".\n' % core_version)
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'sdk_guard.py'), '--tree', spkg])
    if r.returncode:
        sys.exit('sdk_guard blocked the single All In One package')
    files = []
    for dp, _, fns in os.walk(spkg):
        for fn in fns:
            full = os.path.join(dp, fn)
            files.append((full, os.path.relpath(full, spkg).replace('\\', '/')))
    zp = os.path.join(out, SINGLE_ZIP % aiover)
    det_zip(zp, files)
    with zipfile.ZipFile(zp) as zf:
        if zf.testzip():
            sys.exit('single All In One zip integrity failed')
        znames = zf.namelist()
    bad = [x for x in znames if x.lower().endswith('fur02.redfur') or 'yenneferfur' in x.lower()]
    if bad:
        sys.exit('single All In One zip holds third-party names: %s' % bad)
    print('%s: %d files, %.2f MB' % (os.path.basename(zp), len(znames), os.path.getsize(zp) / 1e6))
    return zp


README_SINGLE = """Hairwork for All - All In One %(v)s  (Nexus witcher3 mod 13996; patch 5.0 next-gen, DX12)
by WinterElfeas

ONE mod folder (%(mod)s) with NVIDIA HairWorks strands instead of hair cards for %(n)d characters:
%(chars)s

Same hair as the file "Hairwork for All - All In One" (one folder per character), packed into a single mod folder
so it counts once against the game's mod limit. Install ONE of the two files, never both: remove every mod<Char>Hairworks,
mod000_TrissHairworks, modVesemirScalp and modTrissScalp folder of the split file before you install this one (the same
requirement functions in both would stop the script compiler).

REQUIRED (the game will not start without the first one: the mod carries a script that checks for it)
  1. Hairwork for All - Core, v%(core)s or newer: the file "Hairwork for All - Core" of this same Nexus page
     (https://www.nexusmods.com/witcher3/mods/13996). Copy mod0000HairworkForAllCore into <game>\\mods.
  2. Graphics Control (https://www.nexusmods.com/witcher3/mods/13349): PT hair > "Hair root transparency mode" = Mode 0
     (path-traced hair shows see-through gaps in mode 1).
  3. HairWorks enabled in the game's graphics options.

INSTALL
  Copy the folder %(mod)s from this zip into <game>\\mods (Core first). It also holds the recoloured scalp textures of
  Vesemir and Triss (the game's own head textures, recoloured, and nothing else). One character can not be switched off on
  its own here: use the split file for that.

TRISS
  Triss's hair is built on the "lore-friendly long hair for Triss" mod (Nexus 6457) by its author. Install that mod yourself:
  it is NOT included here, and Triss's hair needs it. A small script checks for it while the game runs: without that mod,
  Triss keeps her vanilla hair (no HairWorks) and the game shows a message that asks for it.
  The folder is named %(mod)s on purpose: the name sorts before modtriss_lorehair, so this mod loads first and the
  lore-friendly hair mod does not override her HairWorks hair. Do not rename it.

LOAD ORDER (mods.settings)
  If you use a mods.settings priority list (Documents\\The Witcher 3\\mods.settings, written by Script Merger or Vortex),
  give %(mod)s a higher priority (a lower number) than modtriss_lorehair and than any other mod that changes the hair
  of these characters: that list overrides the alphabetical order. Keep mod000_CiriWavyHairworks (optional) above it.

CIRI WITH HEADBAND (optional)
  The folder optional\\mod000_CiriWavyHairworks is Ciri's long wavy hair with headband from the mod "Ciri - long wavy hair"
  by bububull (Nexus 7063), converted to HairWorks. Install that mod yourself: it is NOT included here, and this folder needs
  it. A small script in the folder checks for it while the game runs: without it, Ciri keeps her vanilla hair and the game
  shows a message that asks for it.
  It changes the same Ciri template as %(mod)s; installed next to it, the headband one wins (mod000_C sorts before
  mod000_H). Copy the mod000_CiriWavyHairworks folder itself into <game>\\mods, not the optional folder.

Mods that replace the same hair templates of these characters conflict (the mod with the higher priority wins).
Made with Hairwork Studio (same Nexus page). Uses NVIDIA HairWorks, NVIDIA GameWorks technology, (c) NVIDIA Corporation.
No third-party mod files are included.
"""


def check_mod_dir(md):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import w3lib
    name = os.path.basename(md)
    c = os.path.join(md, 'content')
    errs = []
    for need in ('blob0.bundle', 'metadata.store'):
        if not os.path.isfile(os.path.join(c, need)):
            errs.append('%s lacks content/%s' % (name, need))
    if errs:
        return errs
    sd = os.path.join(c, 'scripts', 'local')
    if not (os.path.isdir(sd) and any(f.startswith('hwfa_req_') for f in os.listdir(sd))):
        errs.append('%s: Core requirement script missing' % name)
    for f in os.listdir(c):
        if f.endswith('.bundle'):
            b = os.path.join(c, f)
            for e in w3lib.bundle_entries(b):
                if e.name.endswith('.redfur'):
                    data = w3lib.bundle_read(b, e)
                    if b'importFile' in data or b'px.apx' in data:
                        errs.append('%s: %s carries importFile / third-party skeleton metadata' % (name, e.name))
    return errs


def build_allinone(out, aiover, core_version, jobs, det_zip, scan):
    py = sys.executable
    cfgs = configs()
    stage = os.path.join(ROOT, 'build', 'aio', aiover)
    pkg = os.path.join(stage, 'pkg')
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(pkg)
    env = dict(os.environ, HWFA_MOD_OUT=pkg, HWFA_SKELETON='game')

    def one(t):
        ch, cfg, _mod = t
        log = os.path.join(stage, cfg + '.log')
        with open(log, 'w', encoding='utf-8') as fh:
            r = subprocess.run([py, os.path.join(ROOT, 'tools', 'hairworks', 'hairpipe.py'), cfg, 'build', '--release'] + (['--lore-guard'] if ch == 'triss' else []) + (['--mod', _mod] if ch in AIO_MOD_NAMES else []),
                               cwd=ROOT, env=env, stdout=fh, stderr=subprocess.STDOUT)
        print('  %-10s %-40s %s' % (ch, cfg, 'ok' if r.returncode == 0 else 'FAILED (see %s)' % log), flush=True)
        return r.returncode

    def opt(t):
        key, (cfg, mod) = t
        log = os.path.join(stage, cfg + '.log')
        with open(log, 'w', encoding='utf-8') as fh:
            r = subprocess.run([py, os.path.join(ROOT, 'tools', 'hairworks', 'hairpipe.py'), cfg, 'build', '--release', '--mod-guard', '--mod', mod],
                               cwd=ROOT, env=dict(env, HWFA_MOD_OUT=os.path.join(pkg, OPTIONAL_DIR)), stdout=fh, stderr=subprocess.STDOUT)
        print('  %-10s %-40s %s' % (key, cfg, 'ok (optional)' if r.returncode == 0 else 'FAILED (see %s)' % log), flush=True)
        return r.returncode
    print('All In One: %d release builds (skeleton = game, %d in parallel)' % (len(cfgs), jobs), flush=True)
    with ThreadPoolExecutor(jobs) as ex:
        rcs = list(ex.map(one, cfgs)) + list(ex.map(opt, AIO_OPTIONAL.items()))
    if any(rcs):
        sys.exit('All In One: a build failed')
    game_mods = os.path.join(os.path.dirname(ROOT), 'mods')
    mods = {ch: mod for ch, _c, mod in cfgs}
    for ch, extra in AIO_EXTRA_MODS.items():
        src = os.path.join(game_mods, extra)
        if not os.path.isdir(src):      # not installed in the game: use the scalp_tex.py build folder of the repo
            alt = os.path.join(ROOT, 'build', 'scalp' if ch == 'triss' else 'scalp_' + ch, extra)
            if os.path.isdir(os.path.join(alt, 'content')):
                src = alt
        if not os.path.isdir(src):
            sys.exit('missing %s (build + install it with scalp_tex.py first)' % src)
        print('  scalp %s merged into %s: %s' % (extra, mods[ch], merge_scalp(os.path.join(pkg, mods[ch]), src)), flush=True)
    errs = []
    for _ch, _cfg, mod in cfgs:
        errs += check_mod_dir(os.path.join(pkg, mod))
    for _key, (_cfg, mod) in AIO_OPTIONAL.items():
        md = os.path.join(pkg, OPTIONAL_DIR, mod)
        errs += check_mod_dir(md)
        sd = os.path.join(md, 'content', 'scripts', 'local')
        if not (os.path.isdir(sd) and any(f.startswith('hwfa_') and f.endswith('_guard.ws') for f in os.listdir(sd))):
            errs.append('%s: mod guard script missing' % mod)
    errs += scan(pkg)
    if errs:
        sys.exit('All In One check failed:\n' + '\n'.join(errs))
    chars = '\n'.join('  - ' + AIO_NAMES[ch] for ch in [c for c, _c, _m in cfgs] + list(AIO_OPTIONAL))
    with open(os.path.join(pkg, 'README.txt'), 'w', encoding='utf-8') as fh:
        fh.write(README % dict(v=aiover, chars=chars, core=core_version))
    with open(os.path.join(pkg, 'REQUIREMENTS.txt'), 'w', encoding='utf-8') as fh:
        fh.write('Requires Hairwork for All - Core v%s or newer (Nexus 13996, file "Hairwork for All - Core") and\n'
                 'Graphics Control (Nexus 13349) with PT hair > Hair root transparency mode = Mode 0.\n'
                 'Triss also needs the lore-friendly Triss mod (Nexus 6457), not included.\n'
                 'The optional headband Ciri (optional\\mod000_CiriWavyHairworks) needs "Ciri - long wavy hair" (Nexus 7063), not included.\n' % core_version)
    r = subprocess.run([py, os.path.join(ROOT, 'tools', 'sdk_guard.py'), '--tree', pkg])
    if r.returncode:
        sys.exit('sdk_guard blocked the All In One package')
    files = []
    for dp, _, fns in os.walk(pkg):
        for fn in fns:
            full = os.path.join(dp, fn)
            files.append((full, os.path.relpath(full, pkg).replace('\\', '/')))
    zp = os.path.join(out, 'HairworkForAll_AllInOne_%s.zip' % aiover)
    det_zip(zp, files)
    with zipfile.ZipFile(zp) as zf:
        if zf.testzip():
            sys.exit('All In One zip integrity failed')
        names = zf.namelist()
    # third-party Yennefer mod (modYenneferFurHairworks / fur02.redfur); our own modYenneferHairworks is fine
    bad = [n for n in names if n.lower().endswith('fur02.redfur') or 'yenneferfur' in n.lower()]
    if bad:
        sys.exit('All In One zip holds third-party names: %s' % bad)
    print('%s: %d files, %.2f MB' % (os.path.basename(zp), len(names), os.path.getsize(zp) / 1e6))
    return zp


# ------------------------------------------------------------- in-game check of the single folder (agent test, local)
GAME_MODS = os.path.join(os.path.dirname(ROOT), 'mods')
PARK_MANIFEST = 'parked_split.json'


def _tree_hashes(d):
    import hashlib
    out = {}
    for dp, _, fns in os.walk(d):
        for fn in fns:
            fp = os.path.join(dp, fn)
            with open(fp, 'rb') as fh:
                out[os.path.relpath(fp, d).replace('\\', '/')] = hashlib.sha1(fh.read()).hexdigest()
    return out


def _no_game():
    import settings_guard as SG
    if SG.game_pids():
        sys.exit('witcher3.exe is running: not changing <game>\\mods')


def test_install_single(stage, cfgs):
    """park the installed split folders of the shipped characters (+ the two scalp mods) under stage\\parked_split
    (a rename on the same drive: files untouched, hashes recorded), install stage\\single\\mod000_HairworkForAll and its
    own mods.settings entry (priority of mod000_TrissHairworks, so it beats modtriss_lorehair as the split Triss does)"""
    import settings_guard as SG
    _no_game()
    park = os.path.join(stage, 'parked_split')
    if os.path.exists(os.path.join(park, PARK_MANIFEST)):
        sys.exit('a parked split install is already there: run --restore-split first')
    src = os.path.join(stage, 'single', SINGLE_MOD)
    if not os.path.isdir(src):
        sys.exit('no single build in %s' % src)
    names = [m for _c, _f, m in cfgs] + list(AIO_EXTRA_MODS.values())
    here = [n for n in names if os.path.isdir(os.path.join(GAME_MODS, n))]
    # capture.restore_stale_parks() would move another run's parked copy back into the slot left free here
    stale = [n for n in names if os.path.isdir(os.path.join(ROOT, 'build', 'hair_parked_' + n))]
    if stale:
        sys.exit('other runs parked copies of %s (build\\hair_parked_*): not parking the split install' % stale)
    man ={'parked': {n: _tree_hashes(os.path.join(GAME_MODS, n)) for n in here},
           'tri_prio': SG.mods_entry('mod000_TrissHairworks'), 'had_entry': SG.mods_entry(SINGLE_MOD)}
    os.makedirs(park, exist_ok=True)
    with open(os.path.join(park, PARK_MANIFEST), 'w', encoding='utf-8') as fh:
        json.dump(man, fh, indent=1)
    for n in here:
        os.rename(os.path.join(GAME_MODS, n), os.path.join(park, n))
    shutil.copytree(src, os.path.join(GAME_MODS, SINGLE_MOD))
    if man['had_entry'] is None and man['tri_prio']:
        SG.mods_add(SINGLE_MOD, 1, int(man['tri_prio'][1]), who='build_allinone --test-install-single')
    print('parked %d split folders in %s, installed %s (mods.settings priority %s)'
          % (len(here), park, SINGLE_MOD, man['tri_prio'] and man['tri_prio'][1]))


def restore_split(stage):
    import settings_guard as SG
    _no_game()
    park = os.path.join(stage, 'parked_split')
    with open(os.path.join(park, PARK_MANIFEST), encoding='utf-8') as fh:
        man = json.load(fh)
    inst = os.path.join(GAME_MODS, SINGLE_MOD)
    if os.path.isdir(inst):
        shutil.rmtree(inst)
    if man['had_entry'] is None:
        SG.mods_remove(SINGLE_MOD, who='build_allinone --restore-split')
    bad = []
    for n, hashes in man['parked'].items():
        if os.path.exists(os.path.join(GAME_MODS, n)):
            sys.exit('%s is in <game>\\mods again: not overwriting it (parked copy kept in %s)' % (n, park))
        os.rename(os.path.join(park, n), os.path.join(GAME_MODS, n))
        if _tree_hashes(os.path.join(GAME_MODS, n)) != hashes:
            bad.append(n)
    if bad:
        sys.exit('restored, but these differ from the recorded hashes: %s' % bad)
    os.remove(os.path.join(park, PARK_MANIFEST))
    os.rmdir(park)
    print('restored %d split folders (hashes match), %s removed' % (len(man['parked']), SINGLE_MOD))


if __name__ == '__main__':
    import build_release_hairwork as B
    ap = argparse.ArgumentParser()
    ap.add_argument('--out')
    ap.add_argument('--aio-version', default='0.1.0')
    ap.add_argument('--jobs', type=int, default=3)
    ap.add_argument('--single', action='store_true', help='also build %s (one mod folder), opt-in' % SINGLE_ZIP)
    ap.add_argument('--single-only', action='store_true',
                    help='build only the single zip from the existing split package build\\aio\\<v>\\pkg (no rebuild)')
    ap.add_argument('--test-install-single', action='store_true', help='agent in-game check: park split, install single')
    ap.add_argument('--restore-split', action='store_true', help='undo --test-install-single')
    a = ap.parse_args()
    sys.path.insert(0, os.path.join(ROOT, 'core'))
    import hwcore as K
    stage = os.path.join(ROOT, 'build', 'aio', a.aio_version)
    if a.test_install_single:
        test_install_single(stage, configs())
    elif a.restore_split:
        restore_split(stage)
    elif a.single_only:
        os.makedirs(a.out, exist_ok=True)
        build_single(os.path.join(stage, 'pkg'), a.out, a.aio_version, K.CORE_VERSION, B.det_zip, B.scan, configs())
    else:
        os.makedirs(a.out, exist_ok=True)
        build_allinone(a.out, a.aio_version, K.CORE_VERSION, a.jobs, B.det_zip, B.scan)
        if a.single:
            build_single(os.path.join(stage, 'pkg'), a.out, a.aio_version, K.CORE_VERSION, B.det_zip, B.scan, configs())
