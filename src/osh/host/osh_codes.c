/* osh_codes.c -- names of the refusal codes (OSH_PLATFORM_ABI.md section 5.3, plus the local 236, 246 to 248). */
#include "osh_core.h"

const char *osh_code_name(unsigned code)
{
    static const char *const t[] = {
        [201] = "CAP_LINE", [202] = "CAP_TOKENS", [203] = "CAP_CMDS", [204] = "CAP_PIPELINES", [205] = "CAP_PIPE_LEN",
        [206] = "CAP_WORDS", [207] = "CAP_ASSIGNS", [208] = "CAP_REDIRS", [209] = "CAP_FIELDS", [210] = "CAP_OUT",
        [211] = "CAP_VALUE", [212] = "CAP_VARREQ", [213] = "WORKSPACE_SIZE", [214] = "ABI_MAGIC", [215] = "ABI_VERSION",
        [216] = "ABI_LENGTH", [217] = "ABI_RESERVED", [218] = "REQ_FIELD", [220] = "NUL_BYTE", [221] = "BACKTICK",
        [222] = "GLOB", [223] = "TILDE", [224] = "PARAM_OP", [225] = "SPECIAL_PARAM", [226] = "CMDSUB",
        [227] = "BACKGROUND", [228] = "SUBSHELL", [229] = "HEREDOC", [230] = "CASEEND", [231] = "REDIR_OTHER",
        [232] = "FD_RANGE", [233] = "IF_COMPOUND", [234] = "LOOP", [235] = "CASE", [236] = "GROUP",
        [237] = "FUNCTION", [238] = "NEGATION", [239] = "UNSUPPORTED_BUILTIN", [240] = "IFS_ASSIGN",
        [241] = "SYNTAX_EMPTY_CMD", [242] = "SYNTAX_REDIR_TARGET", [243] = "AMBIGUOUS_REDIRECT", [244] = "VALUE_NUL",
        [245] = "VALUE_GLOB", [246] = "BRACE_EXPANSION", [247] = "POSITIONAL_RANGE", [248] = "SYNTAX_EOF",
    };
    if (code >= sizeof t / sizeof t[0]) return NULL;
    return t[code];
}
