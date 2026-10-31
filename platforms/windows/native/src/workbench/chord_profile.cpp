#include "chord_profile.hpp"
#include "chord_encoding.generated.hpp"

namespace rimes::windows::workbench {
// Portable mapping shared with the macOS and Android isaac2026 profile.
core::Json LegacyBuiltinChordProfile() {
  static const auto value = core::Json::parse(
    R"CHORD({
  "boundaryPolicy": "legacyBatches",
  "formatVersion": 1,
  "id": "builtin.flyyao",
  "leftKeys": "qwertasdfgzxcvb",
  "mappings": [
    {
      "keys": "qy",
      "kind": "syllable",
      "output": "qing"
    },
    {
      "keys": "qn",
      "kind": "syllable",
      "output": "qin"
    },
    {
      "keys": "qm",
      "kind": "syllable",
      "output": "qian"
    },
    {
      "keys": "qu",
      "kind": "syllable",
      "output": "qu"
    },
    {
      "keys": "qi",
      "kind": "syllable",
      "output": "qi"
    },
    {
      "keys": "qp",
      "kind": "syllable",
      "output": "qun"
    },
    {
      "keys": "qjl",
      "kind": "syllable",
      "output": "qiu"
    },
    {
      "keys": "qui",
      "kind": "syllable",
      "output": "quan"
    },
    {
      "keys": "qm.",
      "kind": "syllable",
      "output": "que"
    },
    {
      "keys": "qil",
      "kind": "syllable",
      "output": "qie"
    },
    {
      "keys": "qlm",
      "kind": "syllable",
      "output": "qiao"
    },
    {
      "keys": "qk.",
      "kind": "syllable",
      "output": "qia"
    },
    {
      "keys": "qkm",
      "kind": "syllable",
      "output": "qiong"
    },
    {
      "keys": "quo",
      "kind": "syllable",
      "output": "qiang"
    },
    {
      "keys": "wo",
      "kind": "syllable",
      "output": "wo"
    },
    {
      "keys": "wj",
      "kind": "syllable",
      "output": "wan"
    },
    {
      "keys": "wl",
      "kind": "syllable",
      "output": "wai"
    },
    {
      "keys": "wh",
      "kind": "syllable",
      "output": "wang"
    },
    {
      "keys": "wu",
      "kind": "syllable",
      "output": "wu"
    },
    {
      "keys": "wio",
      "kind": "syllable",
      "output": "wen"
    },
    {
      "keys": "wm,",
      "kind": "syllable",
      "output": "weng"
    },
    {
      "keys": "woj",
      "kind": "syllable",
      "output": "wei"
    },
    {
      "keys": "wjk",
      "kind": "syllable",
      "output": "wa"
    },
    {
      "keys": "ro",
      "kind": "syllable",
      "output": "ruo"
    },
    {
      "keys": "rj",
      "kind": "syllable",
      "output": "ran"
    },
    {
      "keys": "rk",
      "kind": "syllable",
      "output": "rao"
    },
    {
      "keys": "rh",
      "kind": "syllable",
      "output": "rang"
    },
    {
      "keys": "ru",
      "kind": "syllable",
      "output": "ru"
    },
    {
      "keys": "rp",
      "kind": "syllable",
      "output": "run"
    },
    {
      "keys": "ri",
      "kind": "syllable",
      "output": "ri"
    },
    {
      "keys": "rij",
      "kind": "syllable",
      "output": "re"
    },
    {
      "keys": "rui",
      "kind": "syllable",
      "output": "ruan"
    },
    {
      "keys": "rkm",
      "kind": "syllable",
      "output": "rong"
    },
    {
      "keys": "rio",
      "kind": "syllable",
      "output": "ren"
    },
    {
      "keys": "rm,",
      "kind": "syllable",
      "output": "reng"
    },
    {
      "keys": "r,.",
      "kind": "syllable",
      "output": "rui"
    },
    {
      "keys": "rkl",
      "kind": "syllable",
      "output": "rou"
    },
    {
      "keys": "ty",
      "kind": "syllable",
      "output": "ting"
    },
    {
      "keys": "to",
      "kind": "syllable",
      "output": "tuo"
    },
    {
      "keys": "tj",
      "kind": "syllable",
      "output": "tan"
    },
    {
      "keys": "tk",
      "kind": "syllable",
      "output": "tao"
    },
    {
      "keys": "tl",
      "kind": "syllable",
      "output": "tai"
    },
    {
      "keys": "tm",
      "kind": "syllable",
      "output": "tian"
    },
    {
      "keys": "th",
      "kind": "syllable",
      "output": "tang"
    },
    {
      "keys": "ti",
      "kind": "syllable",
      "output": "ti"
    },
    {
      "keys": "tu",
      "kind": "syllable",
      "output": "tu"
    },
    {
      "keys": "tp",
      "kind": "syllable",
      "output": "tun"
    },
    {
      "keys": "tij",
    )CHORD"
    R"CHORD(  "kind": "syllable",
      "output": "te"
    },
    {
      "keys": "tui",
      "kind": "syllable",
      "output": "tuan"
    },
    {
      "keys": "tjk",
      "kind": "syllable",
      "output": "ta"
    },
    {
      "keys": "tkm",
      "kind": "syllable",
      "output": "tong"
    },
    {
      "keys": "tm,",
      "kind": "syllable",
      "output": "teng"
    },
    {
      "keys": "toj",
      "kind": "syllable",
      "output": "tei"
    },
    {
      "keys": "til",
      "kind": "syllable",
      "output": "tie"
    },
    {
      "keys": "tlm",
      "kind": "syllable",
      "output": "tiao"
    },
    {
      "keys": "t,.",
      "kind": "syllable",
      "output": "tui"
    },
    {
      "keys": "tkl",
      "kind": "syllable",
      "output": "tou"
    },
    {
      "keys": "so",
      "kind": "syllable",
      "output": "suo"
    },
    {
      "keys": "sj",
      "kind": "syllable",
      "output": "san"
    },
    {
      "keys": "sk",
      "kind": "syllable",
      "output": "sao"
    },
    {
      "keys": "sl",
      "kind": "syllable",
      "output": "sai"
    },
    {
      "keys": "sh",
      "kind": "syllable",
      "output": "sang"
    },
    {
      "keys": "su",
      "kind": "syllable",
      "output": "su"
    },
    {
      "keys": "si",
      "kind": "syllable",
      "output": "si"
    },
    {
      "keys": "sp",
      "kind": "syllable",
      "output": "sun"
    },
    {
      "keys": "sij",
      "kind": "syllable",
      "output": "se"
    },
    {
      "keys": "sjk",
      "kind": "syllable",
      "output": "sa"
    },
    {
      "keys": "sio",
      "kind": "syllable",
      "output": "sen"
    },
    {
      "keys": "sm,",
      "kind": "syllable",
      "output": "seng"
    },
    {
      "keys": "soj",
      "kind": "syllable",
      "output": "sei"
    },
    {
      "keys": "skl",
      "kind": "syllable",
      "output": "sou"
    },
    {
      "keys": "sui",
      "kind": "syllable",
      "output": "suan"
    },
    {
      "keys": "skm",
      "kind": "syllable",
      "output": "song"
    },
    {
      "keys": "s,.",
      "kind": "syllable",
      "output": "sui"
    },
    {
      "keys": "dy",
      "kind": "syllable",
      "output": "ding"
    },
    {
      "keys": "do",
      "kind": "syllable",
      "output": "duo"
    },
    {
      "keys": "dj",
      "kind": "syllable",
      "output": "dan"
    },
    {
      "keys": "dk",
      "kind": "syllable",
      "output": "dao"
    },
    {
      "keys": "dl",
      "kind": "syllable",
      "output": "dai"
    },
    {
      "keys": "dm",
      "kind": "syllable",
      "output": "dian"
    },
    {
      "keys": "dh",
      "kind": "syllable",
      "output": "dang"
    },
    {
      "keys": "di",
      "kind": "syllable",
      "output": "di"
    },
    {
      "keys": "du",
      "kind": "syllable",
      "output": "du"
    },
    {
      "keys": "dp",
      "kind": "syllable",
      "output": "dun"
    },
    {
      "keys": "dij",
      "kind": "syllable",
      "output": "de"
    },
    {
      "keys": "dui",
      "kind": "syllable",
      "output": "duan"
    },
    {
      "keys": "djk",
      "kind": "syllable",
      "output": "da"
    },
    {
      "keys": "dkm",
      "kind": "syllable",
      "output": "dong"
    },
    {
      "keys": "d,.",
      "kind": "syllable",
      "output": "dui"
    },
    {
      "keys": "dkl",
      "kind": "syllable",
      "output": "dou"
    },
    {
      "keys": "doj",
      "kind": "syllable",
      "output": "dei"
    },
    {
      "keys": "dlm",
      "kind": "syllable",
      "output": "diao"
    },
    {
      "keys": "djl",
      "kind": "syllable",
      "output": "diu"
    },
    {
      "keys": "dm,",
      "kind": "syllable",
      "output": "deng"
    },
    {
      "keys": "dil",
      "kind": "syllable",
      "output": "die"
    },
    {
      "keys": "dk.",
      "kind": "syllable",
      "output": "dia"
    },
    {
      "keys": "fj",
)CHORD"
    R"CHORD(      "kind": "syllable",
      "output": "fan"
    },
    {
      "keys": "fh",
      "kind": "syllable",
      "output": "fang"
    },
    {
      "keys": "fo",
      "kind": "syllable",
      "output": "fo"
    },
    {
      "keys": "fu",
      "kind": "syllable",
      "output": "fu"
    },
    {
      "keys": "foj",
      "kind": "syllable",
      "output": "fei"
    },
    {
      "keys": "fjk",
      "kind": "syllable",
      "output": "fa"
    },
    {
      "keys": "fio",
      "kind": "syllable",
      "output": "fen"
    },
    {
      "keys": "fm,",
      "kind": "syllable",
      "output": "feng"
    },
    {
      "keys": "fkl",
      "kind": "syllable",
      "output": "fou"
    },
    {
      "keys": "go",
      "kind": "syllable",
      "output": "guo"
    },
    {
      "keys": "gj",
      "kind": "syllable",
      "output": "gan"
    },
    {
      "keys": "gk",
      "kind": "syllable",
      "output": "gao"
    },
    {
      "keys": "gl",
      "kind": "syllable",
      "output": "gai"
    },
    {
      "keys": "gh",
      "kind": "syllable",
      "output": "gang"
    },
    {
      "keys": "gu",
      "kind": "syllable",
      "output": "gu"
    },
    {
      "keys": "gp",
      "kind": "syllable",
      "output": "gun"
    },
    {
      "keys": "gy",
      "kind": "syllable",
      "output": "guai"
    },
    {
      "keys": "gkm",
      "kind": "syllable",
      "output": "gong"
    },
    {
      "keys": "gij",
      "kind": "syllable",
      "output": "ge"
    },
    {
      "keys": "gui",
      "kind": "syllable",
      "output": "guan"
    },
    {
      "keys": "gjk",
      "kind": "syllable",
      "output": "ga"
    },
    {
      "keys": "gio",
      "kind": "syllable",
      "output": "gen"
    },
    {
      "keys": "g,.",
      "kind": "syllable",
      "output": "gui"
    },
    {
      "keys": "gkl",
      "kind": "syllable",
      "output": "gou"
    },
    {
      "keys": "goj",
      "kind": "syllable",
      "output": "gei"
    },
    {
      "keys": "guo",
      "kind": "syllable",
      "output": "guang"
    },
    {
      "keys": "gk.",
      "kind": "syllable",
      "output": "gua"
    },
    {
      "keys": "gm,",
      "kind": "syllable",
      "output": "geng"
    },
    {
      "keys": "zo",
      "kind": "syllable",
      "output": "zuo"
    },
    {
      "keys": "zj",
      "kind": "syllable",
      "output": "zan"
    },
    {
      "keys": "zk",
      "kind": "syllable",
      "output": "zao"
    },
    {
      "keys": "zl",
      "kind": "syllable",
      "output": "zai"
    },
    {
      "keys": "zh",
      "kind": "syllable",
      "output": "zang"
    },
    {
      "keys": "zu",
      "kind": "syllable",
      "output": "zu"
    },
    {
      "keys": "zp",
      "kind": "syllable",
      "output": "zun"
    },
    {
      "keys": "zi",
      "kind": "syllable",
      "output": "zi"
    },
    {
      "keys": "zij",
      "kind": "syllable",
      "output": "ze"
    },
    {
      "keys": "zui",
      "kind": "syllable",
      "output": "zuan"
    },
    {
      "keys": "zjk",
      "kind": "syllable",
      "output": "za"
    },
    {
      "keys": "zio",
      "kind": "syllable",
      "output": "zen"
    },
    {
      "keys": "zkm",
      "kind": "syllable",
      "output": "zong"
    },
    {
      "keys": "z,.",
      "kind": "syllable",
      "output": "zui"
    },
    {
      "keys": "zkl",
      "kind": "syllable",
      "output": "zou"
    },
    {
      "keys": "zoj",
      "kind": "syllable",
      "output": "zei"
    },
    {
      "keys": "zm,",
      "kind": "syllable",
      "output": "zeng"
    },
    {
      "keys": "xy",
      "kind": "syllable",
      "output": "xing"
    },
    {
      "keys": "xn",
      "kind": "syllable",
      "output": "xin"
    },
    {
      "keys": "xm",
      "kind": "syllable",
      "output": "xian"
    },
    {
      "keys": "xu",
      "kind": "syllable",
      "output": "xu"
    },
    {
      "keys": "xp",
  )CHORD"
    R"CHORD(    "kind": "syllable",
      "output": "xun"
    },
    {
      "keys": "xi",
      "kind": "syllable",
      "output": "xi"
    },
    {
      "keys": "xui",
      "kind": "syllable",
      "output": "xuan"
    },
    {
      "keys": "xil",
      "kind": "syllable",
      "output": "xie"
    },
    {
      "keys": "xjl",
      "kind": "syllable",
      "output": "xiu"
    },
    {
      "keys": "xk.",
      "kind": "syllable",
      "output": "xia"
    },
    {
      "keys": "xkm",
      "kind": "syllable",
      "output": "xiong"
    },
    {
      "keys": "xlm",
      "kind": "syllable",
      "output": "xiao"
    },
    {
      "keys": "xuo",
      "kind": "syllable",
      "output": "xiang"
    },
    {
      "keys": "xm.",
      "kind": "syllable",
      "output": "xue"
    },
    {
      "keys": "co",
      "kind": "syllable",
      "output": "cuo"
    },
    {
      "keys": "cj",
      "kind": "syllable",
      "output": "can"
    },
    {
      "keys": "ck",
      "kind": "syllable",
      "output": "cao"
    },
    {
      "keys": "cl",
      "kind": "syllable",
      "output": "cai"
    },
    {
      "keys": "ch",
      "kind": "syllable",
      "output": "cang"
    },
    {
      "keys": "ci",
      "kind": "syllable",
      "output": "ci"
    },
    {
      "keys": "cu",
      "kind": "syllable",
      "output": "cu"
    },
    {
      "keys": "cp",
      "kind": "syllable",
      "output": "cun"
    },
    {
      "keys": "cij",
      "kind": "syllable",
      "output": "ce"
    },
    {
      "keys": "cui",
      "kind": "syllable",
      "output": "cuan"
    },
    {
      "keys": "cjk",
      "kind": "syllable",
      "output": "ca"
    },
    {
      "keys": "c,.",
      "kind": "syllable",
      "output": "cui"
    },
    {
      "keys": "ckl",
      "kind": "syllable",
      "output": "cou"
    },
    {
      "keys": "ckm",
      "kind": "syllable",
      "output": "cong"
    },
    {
      "keys": "cio",
      "kind": "syllable",
      "output": "cen"
    },
    {
      "keys": "cm,",
      "kind": "syllable",
      "output": "ceng"
    },
    {
      "keys": "vj",
      "kind": "syllable",
      "output": "zhan"
    },
    {
      "keys": "vk",
      "kind": "syllable",
      "output": "zhao"
    },
    {
      "keys": "vl",
      "kind": "syllable",
      "output": "zhai"
    },
    {
      "keys": "vh",
      "kind": "syllable",
      "output": "zhang"
    },
    {
      "keys": "vi",
      "kind": "syllable",
      "output": "zhi"
    },
    {
      "keys": "vu",
      "kind": "syllable",
      "output": "zhu"
    },
    {
      "keys": "vo",
      "kind": "syllable",
      "output": "zhuo"
    },
    {
      "keys": "vp",
      "kind": "syllable",
      "output": "zhun"
    },
    {
      "keys": "vy",
      "kind": "syllable",
      "output": "zhuai"
    },
    {
      "keys": "vij",
      "kind": "syllable",
      "output": "zhe"
    },
    {
      "keys": "vui",
      "kind": "syllable",
      "output": "zhuan"
    },
    {
      "keys": "vjk",
      "kind": "syllable",
      "output": "zha"
    },
    {
      "keys": "vkm",
      "kind": "syllable",
      "output": "zhong"
    },
    {
      "keys": "vuo",
      "kind": "syllable",
      "output": "zhuang"
    },
    {
      "keys": "vio",
      "kind": "syllable",
      "output": "zhen"
    },
    {
      "keys": "vm,",
      "kind": "syllable",
      "output": "zheng"
    },
    {
      "keys": "voj",
      "kind": "syllable",
      "output": "zhei"
    },
    {
      "keys": "v,.",
      "kind": "syllable",
      "output": "zhui"
    },
    {
      "keys": "vkl",
      "kind": "syllable",
      "output": "zhou"
    },
    {
      "keys": "vk.",
      "kind": "syllable",
      "output": "zhua"
    },
    {
      "keys": "by",
      "kind": "syllable",
      "output": "bing"
    },
    {
      "keys": "bj",
      "kind": "syllable",
      "output": "ban"
    },
    {
      "keys": "bk",
      "kind": "syllable",
      "output": "bao"
    },
   )CHORD"
    R"CHORD( {
      "keys": "bl",
      "kind": "syllable",
      "output": "bai"
    },
    {
      "keys": "bn",
      "kind": "syllable",
      "output": "bin"
    },
    {
      "keys": "bm",
      "kind": "syllable",
      "output": "bian"
    },
    {
      "keys": "bh",
      "kind": "syllable",
      "output": "bang"
    },
    {
      "keys": "bi",
      "kind": "syllable",
      "output": "bi"
    },
    {
      "keys": "bu",
      "kind": "syllable",
      "output": "bu"
    },
    {
      "keys": "bjk",
      "kind": "syllable",
      "output": "ba"
    },
    {
      "keys": "bio",
      "kind": "syllable",
      "output": "ben"
    },
    {
      "keys": "bm,",
      "kind": "syllable",
      "output": "beng"
    },
    {
      "keys": "boj",
      "kind": "syllable",
      "output": "bei"
    },
    {
      "keys": "blm",
      "kind": "syllable",
      "output": "biao"
    },
    {
      "keys": "bil",
      "kind": "syllable",
      "output": "bie"
    },
    {
      "keys": "dfy",
      "kind": "syllable",
      "output": "ying"
    },
    {
      "keys": "dfj",
      "kind": "syllable",
      "output": "yan"
    },
    {
      "keys": "dfk",
      "kind": "syllable",
      "output": "yao"
    },
    {
      "keys": "dfn",
      "kind": "syllable",
      "output": "yin"
    },
    {
      "keys": "dfh",
      "kind": "syllable",
      "output": "yang"
    },
    {
      "keys": "dfi",
      "kind": "syllable",
      "output": "yi"
    },
    {
      "keys": "dfu",
      "kind": "syllable",
      "output": "yu"
    },
    {
      "keys": "dfp",
      "kind": "syllable",
      "output": "yun"
    },
    {
      "keys": "dfij",
      "kind": "syllable",
      "output": "ye"
    },
    {
      "keys": "dfui",
      "kind": "syllable",
      "output": "yuan"
    },
    {
      "keys": "dfkm",
      "kind": "syllable",
      "output": "yong"
    },
    {
      "keys": "dfm.",
      "kind": "syllable",
      "output": "yue"
    },
    {
      "keys": "dfkl",
      "kind": "syllable",
      "output": "you"
    },
    {
      "keys": "dfjk",
      "kind": "syllable",
      "output": "ya"
    },
    {
      "keys": "efo",
      "kind": "syllable",
      "output": "shuo"
    },
    {
      "keys": "efj",
      "kind": "syllable",
      "output": "shan"
    },
    {
      "keys": "efk",
      "kind": "syllable",
      "output": "shao"
    },
    {
      "keys": "efl",
      "kind": "syllable",
      "output": "shai"
    },
    {
      "keys": "efh",
      "kind": "syllable",
      "output": "shang"
    },
    {
      "keys": "efi",
      "kind": "syllable",
      "output": "shi"
    },
    {
      "keys": "efu",
      "kind": "syllable",
      "output": "shu"
    },
    {
      "keys": "efp",
      "kind": "syllable",
      "output": "shun"
    },
    {
      "keys": "efy",
      "kind": "syllable",
      "output": "shuai"
    },
    {
      "keys": "efk.",
      "kind": "syllable",
      "output": "shua"
    },
    {
      "keys": "efij",
      "kind": "syllable",
      "output": "she"
    },
    {
      "keys": "efui",
      "kind": "syllable",
      "output": "shuan"
    },
    {
      "keys": "efuo",
      "kind": "syllable",
      "output": "shuang"
    },
    {
      "keys": "efio",
      "kind": "syllable",
      "output": "shen"
    },
    {
      "keys": "efm,",
      "kind": "syllable",
      "output": "sheng"
    },
    {
      "keys": "ef,.",
      "kind": "syllable",
      "output": "shui"
    },
    {
      "keys": "efkl",
      "kind": "syllable",
      "output": "shou"
    },
    {
      "keys": "efjk",
      "kind": "syllable",
      "output": "sha"
    },
    {
      "keys": "ero",
      "kind": "syllable",
      "output": "chuo"
    },
    {
      "keys": "erj",
      "kind": "syllable",
      "output": "chan"
    },
    {
      "keys": "erk",
      "kind": "syllable",
      "output": "chao"
    },
    {
      "keys": "erl",
      "kind": "syllable",
      "output": "chai"
    },
    {
      "keys": "erh",
      )CHORD"
    R"CHORD("kind": "syllable",
      "output": "chang"
    },
    {
      "keys": "eri",
      "kind": "syllable",
      "output": "chi"
    },
    {
      "keys": "eru",
      "kind": "syllable",
      "output": "chu"
    },
    {
      "keys": "ery",
      "kind": "syllable",
      "output": "chuai"
    },
    {
      "keys": "erp",
      "kind": "syllable",
      "output": "chun"
    },
    {
      "keys": "erij",
      "kind": "syllable",
      "output": "che"
    },
    {
      "keys": "erui",
      "kind": "syllable",
      "output": "chuan"
    },
    {
      "keys": "erkm",
      "kind": "syllable",
      "output": "chong"
    },
    {
      "keys": "eruo",
      "kind": "syllable",
      "output": "chuang"
    },
    {
      "keys": "erio",
      "kind": "syllable",
      "output": "chen"
    },
    {
      "keys": "erm,",
      "kind": "syllable",
      "output": "cheng"
    },
    {
      "keys": "er,.",
      "kind": "syllable",
      "output": "chui"
    },
    {
      "keys": "erkl",
      "kind": "syllable",
      "output": "chou"
    },
    {
      "keys": "erjk",
      "kind": "syllable",
      "output": "cha"
    },
    {
      "keys": "wry",
      "kind": "syllable",
      "output": "ping"
    },
    {
      "keys": "wrj",
      "kind": "syllable",
      "output": "pan"
    },
    {
      "keys": "wrk",
      "kind": "syllable",
      "output": "pao"
    },
    {
      "keys": "wrl",
      "kind": "syllable",
      "output": "pai"
    },
    {
      "keys": "wrn",
      "kind": "syllable",
      "output": "pin"
    },
    {
      "keys": "wrm",
      "kind": "syllable",
      "output": "pian"
    },
    {
      "keys": "wrh",
      "kind": "syllable",
      "output": "pang"
    },
    {
      "keys": "wri",
      "kind": "syllable",
      "output": "pi"
    },
    {
      "keys": "wru",
      "kind": "syllable",
      "output": "pu"
    },
    {
      "keys": "wro",
      "kind": "syllable",
      "output": "po"
    },
    {
      "keys": "wrio",
      "kind": "syllable",
      "output": "pen"
    },
    {
      "keys": "wrm,",
      "kind": "syllable",
      "output": "peng"
    },
    {
      "keys": "wroj",
      "kind": "syllable",
      "output": "pei"
    },
    {
      "keys": "wril",
      "kind": "syllable",
      "output": "pie"
    },
    {
      "keys": "wrlm",
      "kind": "syllable",
      "output": "piao"
    },
    {
      "keys": "wrjk",
      "kind": "syllable",
      "output": "pa"
    },
    {
      "keys": "xco",
      "kind": "syllable",
      "output": "huo"
    },
    {
      "keys": "xcj",
      "kind": "syllable",
      "output": "han"
    },
    {
      "keys": "xck",
      "kind": "syllable",
      "output": "hao"
    },
    {
      "keys": "xcl",
      "kind": "syllable",
      "output": "hai"
    },
    {
      "keys": "xch",
      "kind": "syllable",
      "output": "hang"
    },
    {
      "keys": "xcu",
      "kind": "syllable",
      "output": "hu"
    },
    {
      "keys": "xci",
      "kind": "syllable",
      "output": "he"
    },
    {
      "keys": "xcy",
      "kind": "syllable",
      "output": "huai"
    },
    {
      "keys": "xcp",
      "kind": "syllable",
      "output": "hun"
    },
    {
      "keys": "xck.",
      "kind": "syllable",
      "output": "hua"
    },
    {
      "keys": "xcij",
      "kind": "syllable",
      "output": "he"
    },
    {
      "keys": "xcui",
      "kind": "syllable",
      "output": "huan"
    },
    {
      "keys": "xckm",
      "kind": "syllable",
      "output": "hong"
    },
    {
      "keys": "xcuo",
      "kind": "syllable",
      "output": "huang"
    },
    {
      "keys": "xcio",
      "kind": "syllable",
      "output": "hen"
    },
    {
      "keys": "xcm,",
      "kind": "syllable",
      "output": "heng"
    },
    {
      "keys": "xc,.",
      "kind": "syllable",
      "output": "hui"
    },
    {
      "keys": "xckl",
      "kind": "syllable",
      "output": "hou"
    },
    {
      "keys": "xcjk",
      "kind": "syllable)CHORD"
    R"CHORD(",
      "output": "ha"
    },
    {
      "keys": "xcoj",
      "kind": "syllable",
      "output": "hei"
    },
    {
      "keys": "cvy",
      "kind": "syllable",
      "output": "jing"
    },
    {
      "keys": "cvn",
      "kind": "syllable",
      "output": "jin"
    },
    {
      "keys": "cvm",
      "kind": "syllable",
      "output": "jian"
    },
    {
      "keys": "cvi",
      "kind": "syllable",
      "output": "ji"
    },
    {
      "keys": "cvp",
      "kind": "syllable",
      "output": "jun"
    },
    {
      "keys": "cvu",
      "kind": "syllable",
      "output": "ju"
    },
    {
      "keys": "cvjl",
      "kind": "syllable",
      "output": "jiu"
    },
    {
      "keys": "cvui",
      "kind": "syllable",
      "output": "juan"
    },
    {
      "keys": "cvm.",
      "kind": "syllable",
      "output": "jue"
    },
    {
      "keys": "cvil",
      "kind": "syllable",
      "output": "jie"
    },
    {
      "keys": "cvlm",
      "kind": "syllable",
      "output": "jiao"
    },
    {
      "keys": "cvk.",
      "kind": "syllable",
      "output": "jia"
    },
    {
      "keys": "cvkm",
      "kind": "syllable",
      "output": "jiong"
    },
    {
      "keys": "cvuo",
      "kind": "syllable",
      "output": "jiang"
    },
    {
      "keys": "sfj",
      "kind": "syllable",
      "output": "kan"
    },
    {
      "keys": "sfk",
      "kind": "syllable",
      "output": "kao"
    },
    {
      "keys": "sfl",
      "kind": "syllable",
      "output": "kai"
    },
    {
      "keys": "sfh",
      "kind": "syllable",
      "output": "kang"
    },
    {
      "keys": "sfu",
      "kind": "syllable",
      "output": "ku"
    },
    {
      "keys": "sfy",
      "kind": "syllable",
      "output": "kuai"
    },
    {
      "keys": "sfp",
      "kind": "syllable",
      "output": "kun"
    },
    {
      "keys": "sfo",
      "kind": "syllable",
      "output": "kuo"
    },
    {
      "keys": "sfk.",
      "kind": "syllable",
      "output": "kua"
    },
    {
      "keys": "sfij",
      "kind": "syllable",
      "output": "ke"
    },
    {
      "keys": "sfui",
      "kind": "syllable",
      "output": "kuan"
    },
    {
      "keys": "sfjk",
      "kind": "syllable",
      "output": "ka"
    },
    {
      "keys": "sfkm",
      "kind": "syllable",
      "output": "kong"
    },
    {
      "keys": "sfuo",
      "kind": "syllable",
      "output": "kuang"
    },
    {
      "keys": "sfio",
      "kind": "syllable",
      "output": "ken"
    },
    {
      "keys": "sfm,",
      "kind": "syllable",
      "output": "keng"
    },
    {
      "keys": "sfoj",
      "kind": "syllable",
      "output": "kei"
    },
    {
      "keys": "sf,.",
      "kind": "syllable",
      "output": "kui"
    },
    {
      "keys": "sfkl",
      "kind": "syllable",
      "output": "kou"
    },
    {
      "keys": "sdy",
      "kind": "syllable",
      "output": "ling"
    },
    {
      "keys": "sdo",
      "kind": "syllable",
      "output": "luo"
    },
    {
      "keys": "sdj",
      "kind": "syllable",
      "output": "lan"
    },
    {
      "keys": "sdk",
      "kind": "syllable",
      "output": "lao"
    },
    {
      "keys": "sdl",
      "kind": "syllable",
      "output": "lai"
    },
    {
      "keys": "sdn",
      "kind": "syllable",
      "output": "lin"
    },
    {
      "keys": "sdm",
      "kind": "syllable",
      "output": "lian"
    },
    {
      "keys": "sdh",
      "kind": "syllable",
      "output": "lang"
    },
    {
      "keys": "sdu",
      "kind": "syllable",
      "output": "lu"
    },
    {
      "keys": "sdi",
      "kind": "syllable",
      "output": "li"
    },
    {
      "keys": "sd,.",
      "kind": "syllable",
      "output": "lv"
    },
    {
      "keys": "sdp",
      "kind": "syllable",
      "output": "lun"
    },
    {
      "keys": "sdjl",
      "kind": "syllable",
      "output": "liu"
    },
    {
      "keys": "sdij",
      "kind": "syllable",
      "output": "le"
    },
 )CHORD"
    R"CHORD(   {
      "keys": "sdui",
      "kind": "syllable",
      "output": "luan"
    },
    {
      "keys": "sdm.",
      "kind": "syllable",
      "output": "lve"
    },
    {
      "keys": "sdjk",
      "kind": "syllable",
      "output": "la"
    },
    {
      "keys": "sdkm",
      "kind": "syllable",
      "output": "long"
    },
    {
      "keys": "sdm,",
      "kind": "syllable",
      "output": "leng"
    },
    {
      "keys": "sdoj",
      "kind": "syllable",
      "output": "lei"
    },
    {
      "keys": "sdil",
      "kind": "syllable",
      "output": "lie"
    },
    {
      "keys": "sdlm",
      "kind": "syllable",
      "output": "liao"
    },
    {
      "keys": "sdkl",
      "kind": "syllable",
      "output": "lou"
    },
    {
      "keys": "sduo",
      "kind": "syllable",
      "output": "liang"
    },
    {
      "keys": "dvuo",
      "kind": "syllable",
      "output": "niang"
    },
    {
      "keys": "sdk.",
      "kind": "syllable",
      "output": "lia"
    },
    {
      "keys": "dvy",
      "kind": "syllable",
      "output": "ning"
    },
    {
      "keys": "dvo",
      "kind": "syllable",
      "output": "nuo"
    },
    {
      "keys": "dvj",
      "kind": "syllable",
      "output": "nan"
    },
    {
      "keys": "dvk",
      "kind": "syllable",
      "output": "nao"
    },
    {
      "keys": "dvl",
      "kind": "syllable",
      "output": "nai"
    },
    {
      "keys": "dvn",
      "kind": "syllable",
      "output": "nin"
    },
    {
      "keys": "dvm",
      "kind": "syllable",
      "output": "nian"
    },
    {
      "keys": "dvh",
      "kind": "syllable",
      "output": "nang"
    },
    {
      "keys": "dvi",
      "kind": "syllable",
      "output": "ni"
    },
    {
      "keys": "dvu",
      "kind": "syllable",
      "output": "nu"
    },
    {
      "keys": "dvjl",
      "kind": "syllable",
      "output": "niu"
    },
    {
      "keys": "dvij",
      "kind": "syllable",
      "output": "ne"
    },
    {
      "keys": "dvui",
      "kind": "syllable",
      "output": "nuan"
    },
    {
      "keys": "dvm.",
      "kind": "syllable",
      "output": "nve"
    },
    {
      "keys": "dvjk",
      "kind": "syllable",
      "output": "na"
    },
    {
      "keys": "dvkm",
      "kind": "syllable",
      "output": "nong"
    },
    {
      "keys": "dvio",
      "kind": "syllable",
      "output": "nen"
    },
    {
      "keys": "dvm,",
      "kind": "syllable",
      "output": "neng"
    },
    {
      "keys": "dvoj",
      "kind": "syllable",
      "output": "nei"
    },
    {
      "keys": "dvil",
      "kind": "syllable",
      "output": "nie"
    },
    {
      "keys": "dvlm",
      "kind": "syllable",
      "output": "niao"
    },
    {
      "keys": "dvkl",
      "kind": "syllable",
      "output": "nou"
    },
    {
      "keys": "dv,.",
      "kind": "syllable",
      "output": "nv"
    },
    {
      "keys": "wey",
      "kind": "syllable",
      "output": "ming"
    },
    {
      "keys": "wej",
      "kind": "syllable",
      "output": "man"
    },
    {
      "keys": "wek",
      "kind": "syllable",
      "output": "mao"
    },
    {
      "keys": "wel",
      "kind": "syllable",
      "output": "mai"
    },
    {
      "keys": "wen",
      "kind": "syllable",
      "output": "min"
    },
    {
      "keys": "wem",
      "kind": "syllable",
      "output": "mian"
    },
    {
      "keys": "weh",
      "kind": "syllable",
      "output": "mang"
    },
    {
      "keys": "weu",
      "kind": "syllable",
      "output": "mu"
    },
    {
      "keys": "weo",
      "kind": "syllable",
      "output": "mo"
    },
    {
      "keys": "wei",
      "kind": "syllable",
      "output": "mi"
    },
    {
      "keys": "weij",
      "kind": "syllable",
      "output": "me"
    },
    {
      "keys": "wejk",
      "kind": "syllable",
      "output": "ma"
    },
    {
      "keys": "weio",
      "kind": "syllable",
      "output": "men"
    },
    {
      "keys": "wem,",
  )CHORD"
    R"CHORD(    "kind": "syllable",
      "output": "meng"
    },
    {
      "keys": "weoj",
      "kind": "syllable",
      "output": "mei"
    },
    {
      "keys": "weil",
      "kind": "syllable",
      "output": "mie"
    },
    {
      "keys": "welm",
      "kind": "syllable",
      "output": "miao"
    },
    {
      "keys": "wekl",
      "kind": "syllable",
      "output": "mou"
    },
    {
      "keys": "eij",
      "kind": "syllable",
      "output": "e"
    },
    {
      "keys": "em,",
      "kind": "syllable",
      "output": "eng"
    },
    {
      "keys": "ajk",
      "kind": "syllable",
      "output": "a"
    },
    {
      "keys": "xvo",
      "kind": "syllable",
      "output": "o"
    },
    {
      "keys": "an",
      "kind": "syllable",
      "output": "an"
    },
    {
      "keys": "en",
      "kind": "syllable",
      "output": "en"
    },
    {
      "keys": "xvu",
      "kind": "syllable",
      "output": "ou"
    },
    {
      "keys": "ah",
      "kind": "syllable",
      "output": "ang"
    },
    {
      "keys": "eui",
      "kind": "syllable",
      "output": "er"
    },
    {
      "keys": "jl",
      "kind": "fragment",
      "output": "iu"
    },
    {
      "keys": "ij",
      "kind": "fragment",
      "output": "e"
    },
    {
      "keys": "ui",
      "kind": "fragment",
      "output": "uan"
    },
    {
      "keys": "m.",
      "kind": "fragment",
      "output": "ue"
    },
    {
      "keys": "jk",
      "kind": "fragment",
      "output": "a"
    },
    {
      "keys": "km",
      "kind": "fragment",
      "output": "ong"
    },
    {
      "keys": "uo",
      "kind": "fragment",
      "output": "uang"
    },
    {
      "keys": "io",
      "kind": "fragment",
      "output": "en"
    },
    {
      "keys": "m,",
      "kind": "fragment",
      "output": "eng"
    },
    {
      "keys": "oj",
      "kind": "fragment",
      "output": "ei"
    },
    {
      "keys": "il",
      "kind": "fragment",
      "output": "ie"
    },
    {
      "keys": "lm",
      "kind": "fragment",
      "output": "iao"
    },
    {
      "keys": ",.",
      "kind": "fragment",
      "output": "ui"
    },
    {
      "keys": "kl",
      "kind": "fragment",
      "output": "ou"
    },
    {
      "keys": "k.",
      "kind": "fragment",
      "output": "ia"
    },
    {
      "keys": "df",
      "kind": "fragment",
      "output": "y"
    },
    {
      "keys": "ef",
      "kind": "fragment",
      "output": "sh"
    },
    {
      "keys": "er",
      "kind": "fragment",
      "output": "ch"
    },
    {
      "keys": "xv",
      "kind": "fragment",
      "output": "o"
    },
    {
      "keys": "wr",
      "kind": "fragment",
      "output": "p"
    },
    {
      "keys": "xc",
      "kind": "fragment",
      "output": "h"
    },
    {
      "keys": "cv",
      "kind": "fragment",
      "output": "j"
    },
    {
      "keys": "sf",
      "kind": "fragment",
      "output": "k"
    },
    {
      "keys": "sd",
      "kind": "fragment",
      "output": "l"
    },
    {
      "keys": "dv",
      "kind": "fragment",
      "output": "n"
    },
    {
      "keys": "we",
      "kind": "fragment",
      "output": "m"
    }
  ],
  "name": "isaac2026",
  "outputEncoding": "fullPinyin",
  "rightKeys": "yuiophjklnm,."
})CHORD");
  return value;
}
core::Json BuiltinChordProfile() {
  auto value = LegacyBuiltinChordProfile();
  value["id"] = "21616362-5ee0-4833-a32b-f1dd05c589e7";
  value["outputEncoding"] = "ziranma";
  return value;
}
core::Json EffectiveChordProfile(const core::Json& value) {
  // Only advance the exact maintained Windows template. Old custom snapshots,
  // missing encoding fields and deliberate full-pinyin choices stay untouched.
  if (value.is_null() || value == LegacyBuiltinChordProfile())
    return BuiltinChordProfile();
  return value;
}
std::string ChordProfile::EngineOutput(const ChordMapping& mapping) const {
  if (encoding == "fullPinyin") return mapping.output;
  static const auto tables = core::Json::parse(generated::kChordEncoding);
  const auto& table = tables.at(mapping.kind == "syllable" ? "syllables" : "fragments");
  const auto found = table.find(mapping.output);
  if (found == table.end()) throw std::runtime_error("拼音不能转为自然码。");
  return found->get<std::string>();
}
std::string NaturalChordSchema() { return generated::kNaturalChordSchema; }
}
