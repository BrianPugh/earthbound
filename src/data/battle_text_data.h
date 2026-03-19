/*
 * Battle text SNES addresses — MSG_BTL_* constants.
 *
 * These are SNES ROM addresses of text bytecode scripts within the
 * EBATTLE0-9 and EGOODS0-4 text data blocks. Used as arguments to
 * display_text_from_snes_addr() and display_in_battle_text_addr().
 *
 * Generated from earthbound.yml text_data_symbols section.
 * Each address = block SNES base + symbol offset within the block.
 */
#ifndef DATA_BATTLE_TEXT_DATA_H
#define DATA_BATTLE_TEXT_DATA_H

/* --- EBATTLE0 (base: $EF843F) --- */
#define MSG_BTL_AT_START_NEMURI     0xEF843Fu
#define MSG_BTL_AT_START_FUUIN      0xEF8444u
#define MSG_BTL_AT_START_HEN        0xEF8445u
#define MSG_BTL_RND_ACT_HEN         0xEF845Du
#define MSG_BTL_RND_ACT_KINOKO      0xEF8477u
#define MSG_BTL_TATAKU              0xEF848Cu
#define MSG_BTL_UTU                 0xEF84B6u
#define MSG_BTL_MAMORU              0xEF84C6u
#define MSG_BTL_METAMORPHOSE        0xEF84D4u
#define MSG_BTL_PLAYER_FLEE         0xEF84F3u
#define MSG_BTL_PLAYER_FLEE_NG      0xEF8511u
#define MSG_BTL_CHECK               0xEF8530u
#define MSG_BTL_PSI                 0xEF8543u
#define MSG_BTL_INORU               0xEF89E0u
#define MSG_BTL_THUNDER_SMALL       0xEF8814u
#define MSG_BTL_THUNDER_LARGE       0xEF8823u
#define MSG_BTL_THUNDER_MISS_SE     0xEF8837u

/* --- EBATTLE1 (base: $EF9A47) --- */
#define MSG_BTL_NAKAMA0             0xEF9A47u
#define MSG_BTL_TANEMAKI0           0xEF9A5Eu
#define MSG_BTL_EXPLOSION           0xEF9A7Eu
#define MSG_BTL_BURN                0xEF9A9Eu
#define MSG_BTL_GOODS               0xEF9ABBu
#define MSG_BTL_TIMESTOP            0xEF9B02u
#define MSG_BTL_MANAZASHI           0xEF9B20u
#define MSG_BTL_KAIDENPA            0xEF9B43u
#define MSG_BTL_YORO_KAIDENPA       0xEF9B73u
#define MSG_BTL_GEPPU_IKI           0xEF9B96u
#define MSG_BTL_DOKUBARI            0xEF9BC3u
#define MSG_BTL_DEATH_KISS          0xEF9BE6u
#define MSG_BTL_TUMETAI_IKI         0xEF9C02u
#define MSG_BTL_HOUSHI              0xEF9C30u
#define MSG_BTL_TORITUKI            0xEF9C51u
#define MSG_BTL_YOI_KAORI           0xEF9C7Eu
#define MSG_BTL_KABI_HOUSI          0xEF9CADu
#define MSG_BTL_SHIBARI             0xEF9CD1u
#define MSG_BTL_NENEKI              0xEF9CF1u
#define MSG_BTL_HAEMITU             0xEF9D14u
#define MSG_BTL_OSHIRI_ITO          0xEF9D3Eu
#define MSG_BTL_KOWAI_KOTOBA        0xEF9D62u
#define MSG_BTL_AYASHI_KOTO         0xEF9D81u
#define MSG_BTL_FUUIN               0xEF9DA1u
#define MSG_BTL_TACHIBA_THINK       0xEF9DBDu
#define MSG_BTL_KOGEPPU_IKI         0xEF9DDAu
#define MSG_BTL_TYPHOON             0xEF9E05u
#define MSG_BTL_COFFEE              0xEF9E22u
#define MSG_BTL_MUSIC               0xEF9E47u
#define MSG_BTL_SYOUKA_EKI          0xEF9E69u
#define MSG_BTL_KAMINARI            0xEF9E92u
#define MSG_BTL_FIRE                0xEF9EB4u
#define MSG_BTL_FIRE_BREATH         0xEF9ED7u

/* --- EBATTLE2 (base: $EF7E25) --- */
#define MSG_BTL_PPDOWN              0xEF7E25u
#define MSG_BTL_OKORU               0xEF7E3Eu
#define MSG_BTL_KITANAI_KOTOBA      0xEF7E55u
#define MSG_BTL_ENERGY              0xEF7E88u
#define MSG_BTL_DOKU_KAMITUKI       0xEF7EACu
#define MSG_BTL_YORO_MISSILE        0xEF7ED5u
#define MSG_BTL_MULTI_ATTACK        0xEF7F02u
#define MSG_BTL_MIGAMAE             0xEF7F1Eu
#define MSG_BTL_FIREBALL            0xEF7F32u
#define MSG_BTL_GEKITOTU            0xEF7F5Au
#define MSG_BTL_KARATE              0xEF7F7Bu
#define MSG_BTL_TOMOE               0xEF7F9Au
#define MSG_BTL_BOUSOU              0xEF7FC3u
#define MSG_BTL_KNIFE               0xEF7FE0u
#define MSG_BTL_TOSSIN              0xEF7FFCu
#define MSG_BTL_KAMITUKI            0xEF8010u
#define MSG_BTL_HIKKAKI             0xEF8026u
#define MSG_BTL_SIPPO               0xEF804Bu
#define MSG_BTL_NOSHIKAKARI         0xEF806Du
#define MSG_BTL_KAMIBUKURO          0xEF808Du
#define MSG_BTL_KONBOU              0xEF80ACu
#define MSG_BTL_TATUMAKI            0xEF80C4u
#define MSG_BTL_WATER               0xEF80E4u
#define MSG_BTL_FUTEKI_SMILE        0xEF8109u
#define MSG_BTL_LOUD_SMILE          0xEF812Bu
#define MSG_BTL_NIJIRIYORU          0xEF814Fu
#define MSG_BTL_TUBUYAKI1           0xEF8167u
#define MSG_BTL_TUBUYAKI2           0xEF8186u
#define MSG_BTL_TUBUYAKI3           0xEF81A5u
#define MSG_BTL_KOROBU              0xEF81C4u
#define MSG_BTL_BOOTTO              0xEF81D7u
#define MSG_BTL_JYOUKI              0xEF81F1u
#define MSG_BTL_YOROYORO            0xEF8211u
#define MSG_BTL_FURAFURA            0xEF8226u
#define MSG_BTL_NITANITA            0xEF8239u
#define MSG_BTL_KOKYUU              0xEF825Cu
#define MSG_BTL_AISATU              0xEF8281u
#define MSG_BTL_UNARI               0xEF8299u
#define MSG_BTL_KACHIKACHI          0xEF82BCu
#define MSG_BTL_MABUSII_HIKARI      0xEF82D7u
#define MSG_BTL_BIRIBIRI            0xEF82F7u
#define MSG_BTL_COLD_HAND           0xEF833Eu
#define MSG_BTL_POISON_BREATH       0xEF835Cu
#define MSG_BTL_HAIKI_GAS           0xEF838Au
#define MSG_BTL_LAUGH_HEN           0xEF83A8u
#define MSG_BTL_FUE                 0xEF83CAu
#define MSG_BTL_JUMP_TO_FACE        0xEF83EDu
#define MSG_BTL_CHOU_ONPA           0xEF8413u
#define MSG_BTL_SUIBUN              0xEF7E70u

/* --- EBATTLE3 (base: $EF89FE) --- */
#define MSG_BTL_JIHIBIKI            0xEF89FEu
#define MSG_BTL_OSAETSUKE           0xEF8A18u
#define MSG_BTL_CURSE_WORD          0xEF8A33u
#define MSG_BTL_JIMI                0xEF8A52u
#define MSG_BTL_PENKI               0xEF8A6Fu
#define MSG_BTL_NAGURI_KAKARI       0xEF8A8Cu
#define MSG_BTL_CLAW                0xEF8AA3u
#define MSG_BTL_KUCHIBASHI          0xEF8AC2u
#define MSG_BTL_TSUNO               0xEF8ADDu
#define MSG_BTL_PUNCH               0xEF8AF8u
#define MSG_BTL_PUMPKIN             0xEF8B11u
#define MSG_BTL_BEAM                0xEF8B2Fu
#define MSG_BTL_YARI                0xEF8B4Au
#define MSG_BTL_FUMITSUKE           0xEF8B65u
#define MSG_BTL_FURAFUUPU           0xEF8B89u
#define MSG_BTL_TAIATARI            0xEF8BA8u
#define MSG_BTL_SKATEBOARD          0xEF8BC0u
#define MSG_BTL_KAMITSUKI_DIAMOND   0xEF8BE8u
#define MSG_BTL_KUDAMAKI            0xEF8BFBu
#define MSG_BTL_SEKKYOU             0xEF8C1Du
#define MSG_BTL_SHIKARITSUKE        0xEF8C3Au
#define MSG_BTL_BAD_SMELL           0xEF8C58u
#define MSG_BTL_LOUD_VOICE          0xEF8C75u
#define MSG_BTL_OTAKEBI             0xEF8C92u
#define MSG_BTL_FAKE_DEAD           0xEF8CACu
#define MSG_BTL_YUDAN               0xEF8CC7u
#define MSG_BTL_YUDAN_1             0xEF8CDDu
#define MSG_BTL_YUDAN_2             0xEF8CFBu
#define MSG_BTL_YUDAN_3             0xEF8D17u
#define MSG_BTL_YUDAN_4             0xEF8D2Fu
#define MSG_BTL_YUDAN_LIFEUP        0xEF8D4Cu
#define MSG_BTL_NEBIE_BEAM          0xEF8D72u
#define MSG_BTL_NEUTRALIZE_SPARKLE  0xEF8D9Fu
#define MSG_BTL_MAKITSUKI           0xEF8DC1u
#define MSG_BTL_TO_DIAMOND_DOG      0xEF8DDEu
#define MSG_BTL_WARP_NEAR           0xEF8E27u
#define MSG_BTL_ANTIPSI             0xEF8E3Cu
#define MSG_BTL_HPSUCK              0xEF8E5Eu
#define MSG_BTL_HPSUCKSP            0xEF8E7Eu
#define MSG_BTL_SHIELDKILL          0xEF8E9Eu
#define MSG_BTL_BAD_SMELL_GAS       0xEF8EBEu
#define MSG_BTL_LIGHTNING           0xEF8EE2u
#define MSG_BTL_LIGHTNING_B         0xEF8F17u
#define MSG_BTL_LIGHTNING_C         0xEF8F4Au
#define MSG_BTL_GYIYYIG_3           0xEF8F91u

/* --- EBATTLE4 (base: $EF7186) --- */
#define MSG_BTL_DAIYA_STAT          0xEF7186u
#define MSG_BTL_SHIBIRE_STAT        0xEF7192u
#define MSG_BTL_KIMOCHI_STAT        0xEF71B4u
#define MSG_BTL_MODOKU_STAT         0xEF71CCu
#define MSG_BTL_NEMURI_STAT         0xEF71DFu
#define MSG_BTL_SHIBARA_STAT        0xEF71F6u
#define MSG_BTL_FUUIN_STAT          0xEF720Cu
#define MSG_BTL_GARD_ON             0xEF7249u
#define MSG_BTL_HAEMITU_KUU         0xEF725Au
#define MSG_BTL_HOME_STAT           0xEF727Fu
#define MSG_BTL_HOME_STAT_B         0xEF72A0u
#define MSG_BTL_HOME_STAT_C         0xEF72B9u
#define MSG_BTL_HOME_STAT_D         0xEF72DBu
#define MSG_BTL_TONZURA_BREAK_IN    0xEF72F6u
#define MSG_BTL_TONZURA_BREAK_IN_NG 0xEF72F7u
#define MSG_BTL_TONZURA_BREAK_IN_OK 0xEF733Du
#define MSG_BTL_POO_BREAK_IN        0xEF7415u
#define MSG_BTL_POO_BREAK_IN_2      0xEF743Bu
#define MSG_BTL_POKEY_TALK_A        0xEF745Fu
#define MSG_BTL_POKEY_TALK_B        0xEF749Du
#define MSG_BTL_POKEY_TALK_C        0xEF74B0u
#define MSG_BTL_POKEY_TALK_D        0xEF74C9u
#define MSG_BTL_POKEY_TALK_E        0xEF74E6u
#define MSG_BTL_POKEY_TALK_F        0xEF74FAu
#define MSG_BTL_POKEY_TALK_G        0xEF7514u
#define MSG_BTL_POKEY_TALK_H        0xEF7530u
#define MSG_BTL_POKEY_TALK_I        0xEF7548u
#define MSG_BTL_MYDOG_HOWLING       0xEF7569u
#define MSG_BTL_PICKEY_TALK         0xEF7579u
#define MSG_BTL_TONY_TALK           0xEF7591u
#define MSG_BTL_BALMON_TALK         0xEF7593u
#define MSG_BTL_DAMAGE              0xEF75ABu
#define MSG_BTL_DAMAGE_M            0xEF75C2u
#define MSG_BTL_DAMAGE_SMASH        0xEF75D9u
#define MSG_BTL_DAMAGE_SMASH_M      0xEF75F0u
#define MSG_BTL_DAMAGE_TO_DEATH     0xEF7607u
#define MSG_BTL_SMASH_PLAYER        0xEF7624u
#define MSG_BTL_SMASH_MONSTER       0xEF7630u
#define MSG_BTL_UTU_YOKETA          0xEF763Cu
#define MSG_BTL_TATAKU_YOKETA       0xEF7655u
#define MSG_BTL_KIKANAI             0xEF766Eu
#define MSG_BTL_KIKANAI_B           0xEF7682u
#define MSG_BTL_KIKANAI_C           0xEF76B3u
#define MSG_BTL_KARABURI            0xEF76C7u
#define MSG_BTL_KARABURI_UTSU       0xEF76D8u
#define MSG_BTL_NOT_EXIST           0xEF76FDu
#define MSG_BTL_HPSUCK_ME           0xEF7710u
#define MSG_BTL_HPSUCK_ON           0xEF7729u
#define MSG_BTL_PPSUCK              0xEF773Fu
#define MSG_BTL_PPSUCK_OBJ          0xEF7755u
#define MSG_BTL_KAZE_DAMAGE         0xEF77DBu
#define MSG_BTL_KIMOCHI_DAMAGE      0xEF7768u
#define MSG_BTL_MODOKU_DAMAGE       0xEF7787u
#define MSG_BTL_NISSHA_DAMAGE       0xEF77B1u
#define MSG_BTL_HEAL_NG             0xEF7696u

/* --- EBATTLE5 (base: $EF69A1) --- */
#define MSG_BTL_HPMAX_KAIFUKU       0xEF69A1u
#define MSG_BTL_HP_KAIFUKU          0xEF69BAu
#define MSG_BTL_PP_KAIFUKU          0xEF69D2u
#define MSG_BTL_CHECK_OFFENSE       0xEF69EAu
#define MSG_BTL_CHECK_DEFENSE       0xEF69FFu
#define MSG_BTL_CHECK_ANTI_FIRE     0xEF6A0Du
#define MSG_BTL_CHECK_ANTI_FREEZE   0xEF6A24u
#define MSG_BTL_CHECK_ANTI_FLASH    0xEF6A3Cu
#define MSG_BTL_CHECK_ANTI_PARALYSIS 0xEF6A54u
#define MSG_BTL_CHECK_BRAIN_LEVEL_0 0xEF6A6Cu
#define MSG_BTL_CHECK_BRAIN_LEVEL_3 0xEF6A7Fu
#define MSG_BTL_METAMORPHOSE_OK     0xEF6A99u
#define MSG_BTL_METAMORPHOSE_NG     0xEF6AB3u
#define MSG_BTL_DAIYA_ON            0xEF6AC7u
#define MSG_BTL_SHIBIRE_ON          0xEF6AE0u
#define MSG_BTL_MODOKU_ON           0xEF6B18u
#define MSG_BTL_KAZE_ON             0xEF6B2Fu
#define MSG_BTL_KIMOCHI_ON          0xEF6AFBu
#define MSG_BTL_KINOKO_ON           0xEF6B81u
#define MSG_BTL_TORITSU_ON          0xEF6B98u
#define MSG_BTL_NAMIDA_ON           0xEF6BBBu
#define MSG_BTL_SHIBARA_ON          0xEF6BD3u
#define MSG_BTL_KOORI_ON            0xEF6BEFu
#define MSG_BTL_FUUIN_ON            0xEF6C0Bu
#define MSG_BTL_HEN_ON              0xEF6C3Au
#define MSG_BTL_NEMURI_ON           0xEF6C55u
#define MSG_BTL_KIZETU_ON           0xEF6C6Bu
#define MSG_BTL_KIZETU_ON_M         0xEF6D71u
#define MSG_BTL_KIZETU_ON_UGOKANAKU 0xEF6D83u
#define MSG_BTL_KIZETU_ON_OTONASHIKU 0xEF6D96u
#define MSG_BTL_KIZETU_ON_KIESARU   0xEF6DA7u
#define MSG_BTL_KIZETU_ON_KUDAKERU  0xEF6DD8u
#define MSG_BTL_KIZETU_ON_KAKIKIERU 0xEF6DB8u
#define MSG_BTL_KIZETU_ON_HAKAI     0xEF6DF0u
#define MSG_BTL_KIZETU_ON_PONKOTSU  0xEF6E03u
#define MSG_BTL_KIZETU_ON_WARENI    0xEF6E19u
#define MSG_BTL_KIZETU_ON_TSUCHINI  0xEF6E31u
#define MSG_BTL_DAIYA_OFF           0xEF6E4Au
#define MSG_BTL_SHIBIRE_OFF         0xEF6E67u
#define MSG_BTL_KIMOCHI_OFF         0xEF6E81u
#define MSG_BTL_MODOKU_OFF          0xEF6E97u
#define MSG_BTL_KAZE_OFF            0xEF6EBCu
#define MSG_BTL_NAMIDA_OFF          0xEF6ED1u
#define MSG_BTL_SHIBARA_OFF         0xEF6EEDu
#define MSG_BTL_KOORI_STAT          0xEF6F0Bu
#define MSG_BTL_HEN_OFF             0xEF6F1Eu
#define MSG_BTL_NISSYA_OFF          0xEF6F38u
#define MSG_BTL_NEMURI_OFF          0xEF6F54u
#define MSG_BTL_FUUIN_OFF           0xEF6F64u
#define MSG_BTL_IKIKAERI            0xEF6F7Cu
#define MSG_BTL_IKIKAERI_F          0xEF6F8Eu
#define MSG_BTL_SHIELD_ON           0xEF6F9Au
#define MSG_BTL_SHIELD_ADD          0xEF6FBDu
#define MSG_BTL_POWER_ON            0xEF6FD3u
#define MSG_BTL_POWER_ADD           0xEF6FF4u
#define MSG_BTL_PSYCO_ON            0xEF700Cu
#define MSG_BTL_PSYCO_ADD           0xEF7032u
#define MSG_BTL_PSYPOWER_ON         0xEF7050u
#define MSG_BTL_PSYPOWER_ADD        0xEF707Au
#define MSG_BTL_SHIELD_OFF          0xEF7099u
#define MSG_BTL_POWER_TURN          0xEF70B1u
#define MSG_BTL_PSYCO_TURN          0xEF70FAu
#define MSG_BTL_PSYPOWER_TURN       0xEF70D2u
#define MSG_BTL_NEUTRALIZE_RESULT   0xEF7123u
#define MSG_BTL_NEUTRALIZE_METAMORPH 0xEF7142u
#define MSG_BTL_FRANKLIN_TURN       0xEF7160u

/* --- EBATTLE6 (base: $C8F77D) --- */
#define MSG_BTL_OFFENSE_UP          0xC8F77Du
#define MSG_BTL_DEFENSE_UP          0xC8F79Au
#define MSG_BTL_IQ_UP               0xC8F7B8u
#define MSG_BTL_GUTS_UP             0xC8F7D2u
#define MSG_BTL_GUTS_DOWN           0xC8F7EEu
#define MSG_BTL_2GUTS_UP            0xC8F80Au
#define MSG_BTL_SPEED_UP            0xC8F82Fu
#define MSG_BTL_VITA_UP             0xC8F84Cu
#define MSG_BTL_LUCK_UP             0xC8F86Bu
#define MSG_BTL_OFFENSE_DOWN        0xC8F885u
#define MSG_BTL_DEFENSE_DOWN        0xC8F8A2u
#define MSG_BTL_G_HAEMITU_G         0xC8F8C0u
#define MSG_BTL_G_HAEMITU_NG        0xC8F8FDu
#define MSG_BTL_INORU_1             0xC8F935u
#define MSG_BTL_INORU_2             0xC8F956u
#define MSG_BTL_INORU_3             0xC8F973u
#define MSG_BTL_INORU_4             0xC8F992u
#define MSG_BTL_INORU_5             0xC8F9B3u
#define MSG_BTL_INORU_6             0xC8F9CEu
#define MSG_BTL_INORU_7             0xC8F9F0u
#define MSG_BTL_INORU_8             0xC8FA0Du
#define MSG_BTL_INORU_9             0xC8FA2Fu
#define MSG_BTL_INORU_10            0xC8FA51u
#define MSG_BTL_PP_ZERO             0xC8FA7Bu
#define MSG_BTL_ZENKAI              0xC8FA90u
#define MSG_BTL_PSI_CANNOT          0xC8FAB8u
#define MSG_BTL_PSI_CANNOT_MENU     0xC8FAAAu
#define MSG_BTL_KAMINARI_HAZURE     0xC8FAF6u
#define MSG_BTL_PPSUCK_ZERO         0xC8FB05u
#define MSG_BTL_MECHPOKEY_1_TALK    0xC8FB1Bu
#define MSG_BTL_MECHPOKEY_1_TALK_B  0xC8FC2Eu
#define MSG_BTL_MECHPOKEY_2_TALK_2  0xC8FD11u
#define MSG_BTL_POKEY_RUN_AWAY      0xC8FF31u

/* --- EBATTLE7 (base: $C9EE2F) --- */
#define MSG_BTL_GYIYYIG             0xC9EE2Fu
#define MSG_BTL_GYIYYIG_TALK_A      0xC9EE68u
#define MSG_BTL_GYIYYIG_TALK_B      0xC9EE7Fu
#define MSG_BTL_GYIYYIG_TALK_C      0xC9EE9Fu
#define MSG_BTL_GYIYYIG_TALK_D      0xC9EEC7u
#define MSG_BTL_GYIYYIG_TALK_E      0xC9EEDEu
#define MSG_BTL_GYIYYIG_TALK_F      0xC9EF00u
#define MSG_BTL_GYIYYIG_TALK_G      0xC9EFB9u
#define MSG_BTL_GYIYYIG_TALK_H      0xC9EFE1u
#define MSG_BTL_GYIYYIG_TALK_I      0xC9EFF6u
#define MSG_BTL_GYIYYIG_TALK_J      0xC9F020u
#define MSG_BTL_GYIYYIG_TALK_K      0xC9F042u
#define MSG_BTL_GYIYYIG_TALK_L      0xC9F052u
#define MSG_BTL_GYIYYIG_TALK_M      0xC9F074u
#define MSG_BTL_GYIYYIG_TALK_N      0xC9F090u
#define MSG_BTL_INORU_FROM_PAULA_1  0xC9F0B8u
#define MSG_BTL_INORU_FROM_PAULA_2  0xC9F134u
#define MSG_BTL_INORU_FROM_PAULA_3  0xC9F196u
#define MSG_BTL_INORU_FROM_PAULA_4  0xC9F1FDu
#define MSG_BTL_INORU_FROM_PAULA_5  0xC9F25Eu
#define MSG_BTL_INORU_FROM_PAULA_6  0xC9F2BCu
#define MSG_BTL_INORU_FROM_PAULA_7  0xC9F325u
#define MSG_BTL_INORU_FROM_PAULA_8  0xC9F389u
#define MSG_BTL_INORU_FROM_PAULA_9  0xC9F3ECu
#define MSG_BTL_INORU_BACK_TO_PC_1  0xC9F442u
#define MSG_BTL_INORU_BACK_TO_PC_2  0xC9F4A2u
#define MSG_BTL_INORU_BACK_TO_PC_3  0xC9F4F6u
#define MSG_BTL_INORU_BACK_TO_PC_4  0xC9F557u
#define MSG_BTL_INORU_BACK_TO_PC_5  0xC9F5A8u
#define MSG_BTL_INORU_BACK_TO_PC_6  0xC9F60Du
#define MSG_BTL_INORU_BACK_TO_PC_7  0xC9F66Fu
#define MSG_BTL_INORU_BACK_TO_PC_8  0xC9F6DEu
#define MSG_BTL_INORU_BACK_TO_PC_9  0xC9F70Cu
#define MSG_BTL_INORU_BACK_TO_PC_F_1 0xC9F7BBu
#define MSG_BTL_INORU_BACK_TO_PC_F_2 0xC9F804u
#define MSG_BTL_INORU_BACK_TO_PC_F_3 0xC9F84Du
#define MSG_BTL_INORU_DAMAGE_1      0xC9F86Au

/* --- EEVENT4 prayer text (base: bank $C7, used by Giygas prayer actions) --- */
#define MSG_EVT_PRAY_1_NES_MAMA     0xC7B9A1u
#define MSG_EVT_PRAY_2_TONZURA      0xC7BA2Cu
#define MSG_EVT_PRAY_3_PAULA_PAPA   0xC7BAC7u
#define MSG_EVT_PRAY_4_TONY         0xC7BB38u
#define MSG_EVT_PRAY_5_RAMA         0xC7BBF3u
#define MSG_EVT_PRAY_6_FRANK        0xC7BC56u
#define MSG_EVT_PRAY_7_DOSEI        0xC7BC96u

/* --- EBATTLE8 (base: $EF77FD) --- */
#define MSG_BTL_NAKAMA_KITA         0xEF77FDu
#define MSG_BTL_NAKAMA_NO           0xEF7824u
#define MSG_BTL_TANEMAKI_HAETA      0xEF7810u
#define MSG_BTL_TANEMAKI_NO         0xEF7830u
#define MSG_BTL_TIMESTOP_RET        0xEF7843u
#define MSG_BTL_APPEAR              0xEF7858u
#define MSG_BTL_AP_FUSAGARETA       0xEF7866u
#define MSG_BTL_AP_ATTESHIMATTA     0xEF7879u
#define MSG_BTL_AP_TSUKAMATTA       0xEF788Bu
#define MSG_BTL_AP_FINAL4           0xEF789Cu
#define MSG_BTL_AP_FINAL5           0xEF78ABu
#define MSG_BTL_AP_FINAL6           0xEF78B8u
#define MSG_BTL_AP_FINAL7           0xEF78C7u
#define MSG_BTL_SENSEI_PC           0xEF78D8u
#define MSG_BTL_SENSEI_MON          0xEF78F7u
#define MSG_BTL_PLAYER_WIN          0xEF79D7u
#define MSG_BTL_PLAYER_WIN_BOSS     0xEF7A14u
#define MSG_BTL_PLAYER_WIN_FORCE    0xEF7A28u
#define MSG_BTL_MONSTER_WIN         0xEF7A4Du
#define MSG_BTL_LEVEL_UP            0xEF7A66u
#define MSG_BTL_LV_OFFENSE_UP       0xEF7A7Du
#define MSG_BTL_LV_DEFENSE_UP       0xEF7A97u
#define MSG_BTL_LV_SPEED_UP         0xEF7AB1u
#define MSG_BTL_LV_GUTS_UP          0xEF7AC9u
#define MSG_BTL_LV_VITA_UP          0xEF7AE0u
#define MSG_BTL_LV_IQ_UP            0xEF7AFBu
#define MSG_BTL_LV_LUCK_UP          0xEF7B11u
#define MSG_BTL_LV_MAXHP_UP        0xEF7B28u
#define MSG_BTL_LV_MAXPP_UP        0xEF7B46u
#define MSG_BTL_LEARN_PSI           0xEF7B64u
#define MSG_BTL_PRESENT             0xEF7BDFu
#define MSG_BTL_CHECK_PRESENT_GET   0xEF7DD5u

/* --- EGOODS1 (base: $C9F897) --- */
#define MSG_BTL_TLPTBOX_OK          0xC9FE41u  /* offset 0x05AA */
#define MSG_BTL_TLPTBOX_NG          0xC9FE9Du  /* offset 0x0606 */
#define MSG_BTL_TLPTBOX_CANT        0xC9FEE3u  /* offset 0x064C */

/* --- EGOODS0 (base: $C97B6B) --- */
#define MSG_BTL_EAT_SPICE_ATARI     0xC97C9Du  /* condiment matched: great flavor! */
#define MSG_BTL_EAT_SPICE_HAZURE    0xC97CB1u  /* condiment mismatched */

/* --- EGOODS4 (base: $C77DCE) --- */
#define MSG_BTL_GOODS_NO_EFFECT     0xC77DCEu  /* offset 0x0000 */
#define MSG_BTL_GOODS_NO_EFFECT_ON_BTL 0xC77DF0u /* offset 0x0022 */
#define MSG_BTL_EQUIP_OK            0xC77E11u  /* offset 0x0043 */
#define MSG_BTL_EQUIP_NG_WEAPON     0xC77E33u  /* offset 0x0065 */
#define MSG_BTL_EQUIP_NG_ARMOR      0xC77E85u  /* offset 0x00B7 */
#define MSG_BTL_GOODS_WRONG_USER    0xC77EB6u  /* offset 0x00E8 */

#endif /* DATA_BATTLE_TEXT_DATA_H */
