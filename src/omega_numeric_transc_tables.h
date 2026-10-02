#ifndef OMEGA_NUMERIC_TRANSC_TABLES_H
#define OMEGA_NUMERIC_TRANSC_TABLES_H
/*
 * Frozen erfcx(y) = e^(y^2) erfc(y) interval tables of the bounded ERF/GELU
 * contract (docs/numeric/E1_TRANSCENDENTAL_CONTRACT.md). One copy, included by
 * the CPU sequences (src/omega_numeric_transc.c) and by the GB10 kernel
 * builder (src/omega_numeric_divsqrt_gb10.c), so both realizations read the
 * same constants. Moved verbatim from omega_numeric_transc.c; do not edit
 * a value without re-running every erfcx-dependent gate.
 */
/* erfcx(y) = e^(y^2) erfc(y), 0.5 <= y < 11: degree-10 Taylor polynomial
 * about the centre of one of 17 intervals. */
#define ERFCX_N 17
static const float ERFCX_LO[ERFCX_N] = {
    0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 3.5f,
    4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f,
};
static const float ERFCX_C[ERFCX_N] = {
    0.625f, 0.875f, 1.125f, 1.375f, 1.625f, 1.875f, 2.25f, 2.75f, 3.25f, 3.75f,
    4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f,
};
static const float ERFCX_A[ERFCX_N][11] = {
    { 0x8E8B5Bp-24f, -0xDD5E87p-25f, 0x92BBA1p-25f, -0xACE1AEp-26f, 0xB96A35p-27f, -0xB7E72Fp-28f, 0xAA97F4p-29f, -0x953FA8p-30f, 0xF7E81Ep-32f, -0xC477CDp-33f, 0x953558p-34f },  /* c = 0.625 */
    { 0xEDBA3Fp-25f, -0xA1B4FBp-25f, 0xC077C8p-26f, -0xCEAC34p-27f, 0xCC18E1p-28f, -0xBBCEE9p-29f, 0xA29324p-30f, -0x8559C2p-31f, 0xD077BFp-33f, -0x9BFF38p-34f, 0xE059BEp-36f },  /* c = 0.875 */
    { 0xCA98F0p-25f, -0xF3C59Bp-26f, 0x82F393p-26f, -0x809A1Dp-27f, 0xEA7389p-29f, -0xC8852Fp-30f, 0xA23629p-31f, -0xF9C637p-33f, 0xB7ECD3p-34f, -0x820F1Cp-35f, 0xB13A1Ep-37f },  /* c = 1.125 */
    { 0xAFC47Bp-25f, -0xBCBD32p-26f, 0xB809A1p-27f, -0xA5E6DDp-28f, 0x8BF5D3p-29f, -0xDEF926p-31f, 0xA8D5D0p-32f, -0xF45734p-34f, 0xA9AFACp-35f, -0xE2FD36p-37f, 0x92A769p-38f },  /* c = 1.375 */
    { 0x9AC1FBp-25f, -0x9588F3p-26f, 0x850AD6p-27f, -0xDD00C7p-29f, 0xAD0A15p-30f, -0x80A757p-31f, 0xB6B050p-33f, -0xF8DA1Fp-35f, 0xA32F67p-36f, -0xCEB114p-38f, 0xFD7E48p-40f },  /* c = 1.625 */
    { 0x89F2BAp-25f, -0xF1B2C0p-27f, 0xC5377Fp-28f, -0x977C98p-29f, 0xDCCAC5p-31f, -0x9991BEp-32f, 0xCCDB07p-34f, -0x838706p-35f, 0xA318E2p-37f, -0xC3D30Fp-39f, 0xE42CA4p-41f },  /* c = 1.875 */
    { 0xECA223p-26f, -0xB53869p-27f, 0x830AB4p-28f, -0xB441FDp-30f, 0xED2CABp-32f, -0x95E929p-33f, 0xB6BBA2p-35f, -0xD76BF6p-37f, 0xF63B9Ep-39f, -0x88BD33p-40f, 0x93D7A1p-42f },  /* c = 2.25 */
    { 0xC64F5Bp-26f, -0x8182BCp-27f, 0xA1DCC6p-29f, -0xC274D6p-31f, 0xE16398p-33f, -0xFCCF21p-35f, 0x898DD9p-36f, -0x918959p-38f, 0x95FDAEp-40f, -0x96D195p-42f, 0x942B7Dp-44f },  /* c = 2.75 */
    { 0xAA53D0p-26f, -0xC15429p-28f, 0xD3F6EEp-30f, -0xE125A5p-32f, 0xE842C3p-34f, -0xE92F5Ep-36f, 0xE44178p-38f, -0xDA2E81p-40f, 0xCBEEBCp-42f, -0xBA9E56p-44f, 0xA76049p-46f },  /* c = 3.25 */
    { 0x951579p-26f, -0x955328p-28f, 0x917833p-30f, -0x8A1A57p-32f, 0xFFF819p-35f, -0xE7E6F5p-37f, 0xCDA868p-39f, -0xB2BBC5p-41f, 0x98617Ep-43f, -0xFF1F19p-46f, 0xD1DF72p-48f },  /* c = 3.75 */
    { 0xFAD950p-27f, -0xD51F5Bp-29f, 0xB16096p-31f, -0x90C76Dp-33f, 0xE803C3p-36f, -0xB6AA59p-38f, 0x8D6B52p-40f, -0xD77C75p-43f, 0xA1AA7Ep-45f, -0xEF04AAp-48f, 0xAE325Ap-50f },  /* c = 4.5 */
    { 0xCEC548p-27f, -0x91C654p-29f, 0xCA9296p-32f, -0x8ACBB7p-34f, 0xBBA7A4p-37f, -0xFA7237p-40f, 0xA50BF1p-42f, -0xD6EBD6p-45f, 0x8A4E6Cp-47f, -0xB0000Fp-50f, 0xDD84DDp-53f },  /* c = 5.5 */
    { 0xAFBAE2p-27f, -0xD37168p-30f, 0xFBAF7Cp-33f, -0x943C40p-35f, 0xACD949p-38f, -0xC791FFp-41f, 0xE438D1p-44f, -0x8147A3p-46f, 0x91233Dp-49f, -0xA1801Ap-52f, 0xB2285Ep-55f },  /* c = 6.5 */
    { 0x98BA18p-27f, -0xA0222Bp-30f, 0xA683DEp-33f, -0xABC1D1p-36f, 0xAFC535p-39f, -0xB27E5Ep-42f, 0xB3E516p-45f, -0xB3F8A7p-48f, 0xB2BF9Dp-51f, -0xB04751p-54f, 0xACA33Bp-57f },  /* c = 7.5 */
    { 0x8703C0p-27f, -0xFABE57p-31f, 0xE75118p-34f, -0xD40795p-37f, 0xC12136p-40f, -0xAED44Cp-43f, 0x9D4DC1p-46f, -0x8CB120p-49f, 0xFA31DDp-53f, -0xDD2E77p-56f, 0xC26CBBp-59f },  /* c = 8.5 */
    { 0xF1EDCEp-28f, -0xC98846p-31f, 0xA6FE9Ap-34f, -0x89A7FAp-37f, 0xE1C826p-41f, -0xB83A53p-44f, 0x9592F6p-47f, -0xF1AFD8p-51f, 0xC251B4p-54f, -0x9B7D7Ap-57f, 0xF7AB79p-61f },  /* c = 9.5 */
    { 0xDB1A53p-28f, -0xA5745Ep-31f, 0xF8CB98p-35f, -0xBA40ACp-38f, 0x8AD6E6p-41f, -0xCE1F7Ep-45f, 0x985F41p-48f, -0xE05A4Cp-52f, 0xA47FF1p-55f, -0xF04377p-59f, 0xAEC450p-62f },  /* c = 10.5 */
};

/* a0 = erfcx(c) - (FP32 a0), from the same 120-digit computation */
static const float ERFCX_A0LO[ERFCX_N] = {
    -0xFEC2A1p-50f, 0xE409ACp-50f, 0x9B149Bp-50f, -0xD0C389p-50f, 0xC8864Fp-51f, 0xED8120p-52f,
    0xD62793p-51f, 0x8FF27Dp-51f, 0x8D4BB4p-51f, -0xC7D6DAp-52f, 0x939E99p-53f, -0xDD7B0Dp-52f,
    0xBA5C5Cp-54f, -0xB272B3p-52f, 0xA3E924p-53f, -0x88206Cp-54f, -0xFF9A64p-53f,
};
#endif
