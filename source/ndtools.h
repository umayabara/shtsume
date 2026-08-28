//
//  ndtools.h
//  shtsume
//
//  Created by Hkijin on 2022/02/03.
//

#ifndef ndtools_h
#define ndtools_h

#include <stdio.h>
#include "shogi.h"
#include "usi.h"
#include "dtools.h"
#include "shtsume.h"

/*
 * 探索情報の表示
 */
int  search_inf_sprintf  (char *restrict str,
                          const mvlist_t *mvlist,
                          const sdata_t *sdata );
int  search_inf_fprintf  (FILE *restrict stream,
                          const mvlist_t *mvlist,
                          const sdata_t *sdata );
#define SEARCH_INF_PRINTF(mvlist, sdata) \
                        search_inf_fprintf(stdout, mvlist, sdata)
/*
 * 探索着手リストの表示
 */
//mlistの内容表示
int mlist_sprintf   (char *restrict str,
                     const mlist_t *mlist,
                     const sdata_t *sdata );
int mlist_fprintf   (FILE *restrict stream,
                     const mlist_t *mlist,
                     const sdata_t *sdata );
#define MLIST_PRINTF(mlist,sdata)   mlist_fprintf(stdout,(mlist),(sdata))
//mvlistの内容表示
int mvlist_sprintf  (char *restrict str,
                     const mvlist_t *mvlist,
                     const sdata_t *sdata );
int mvlist_fprintf  (FILE *restrict stream,
                     const mvlist_t *mvlist,
                     const sdata_t *sdata );
#define MVLIST_PRINTF(mvlist,sdata)   mvlist_fprintf(stdout,(mvlist),(sdata))

int mvlist_sprintf_with_item (char *restrict str,
                              const mvlist_t *mvlist,
                              const sdata_t *sdata );
int mvlist_fprintf_with_item (FILE *restrict stream,
                              const mvlist_t *mvlist,
                              const sdata_t *sdata );

#define MVLIST_PRINTF_ITEM(mvlist,sdata) \
        mvlist_fprintf_with_item(stdout,(mvlist),(sdata))

int mvlist_fprintf_essence   (FILE *restrict  stream,
                              const mvlist_t *mvlist,
                              const sdata_t  *sdata );
#define MVLIST_PRINTF_ESSENCE(mvlist,sdata) \
        mvlist_fprintf_essence(stdout,(mvlist),(sdata))

/*
 * 詰手順表示
 */

//全手順表示
#define TP_NONE       0
#define TP_ZKEY       (1<<0)           //詰手順にzkeyを含める
#define TP_ALLMOVE    (1<<1)           //詰方全候補手表示

void tsume_print                (const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 unsigned int     flag );

int tsume_fprint                (FILE            *stream,
                                 const sdata_t   *sdata,
                                 tbase_t         *tbase,
                                 unsigned int     flag );

bool tsume_json_variations_fprint(FILE           *stream,
                                  const sdata_t   *sdata,
                                  tbase_t         *tbase,
                                  const move_t    *principal,
                                  unsigned int     principal_length,
                                  bool             research_lines,
                                  bool             analyze_exclusivity,
                                  bool            *principal_valid);
/*
 * branch_plies に 2 以上を渡すと、all_checking_moves の代案のうちDFPN追加探索でも
 * 未解決だったものについて、その代案を指してから branch_plies 手以内に詰みが
 * 無いことを固定深さの全探索で確認する。確認できた代案の status は
 * no_mate_within_horizon となる。完全な不詰証明(dn==0)とは別の、有界な証拠である。
 * node_budget は1候補あたりの探索ノード上限(0で無制限)、seconds は1回の解析全体に
 * 許す秒数(0で無制限)。上限に達した候補は unresolved のまま残す。
 * escape_uniqueness を立てると、不詰と分かった代案について、指定手数以内の詰みを
 * 免れる受方の応手が何通りあるかを数え escape_move_count として報告する。
 * 全受けを試すので探索量が増える。
 */
void tsume_json_set_bounded_no_mate(unsigned int     branch_plies,
                                    uint64_t         node_budget,
                                    double           seconds,
                                    bool             escape_uniqueness);

bool tsume_json_defender_line_fprint(FILE         *stream,
                                     const sdata_t *sdata,
                                     tbase_t       *tbase);
void tsume_json_defender_variations_fprint(FILE         *stream,
                                           const sdata_t *sdata,
                                           tbase_t       *tbase);
        
//初手から局面ごとに表示
void tsume_debug                (const sdata_t   *sdata,
                                 tbase_t         *tbase);

/*
 * kifファイルの生成を行う
 */
void generate_kif_file          (const char      *filename,
                                 const sdata_t   *sdata   ,
                                 tbase_t         *tbase    );

/*
 * 局面表の内容を表示する。
 */

void print_tbase                (const sdata_t   *sdata,
                                 turn_t              tn,
                                 tbase_t         *tbase);

/*
 *  探索手順検証用構造体
 */
typedef struct _mvel_t mvel_t;
struct _mvel_t
{
    short  dp;
    zkey_t zkey;
    mkey_t mkey;
};

#endif /* ndtools_h */
