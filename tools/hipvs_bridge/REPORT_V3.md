# V3 expedited build configuration

Parent: 9426c7150f4f7434acdc41758f1b761fdb4d6fcd.
The user reports the parent passed official gates with build=2.107x, query=4.187x, score 15.495.

Only production change: NN-descent iteration count 30 -> 10. Keep intermediate degree 160, final degree 128, MULTI_CTA, itopk 1024, width 4, persistent worker and OpenMP cap unchanged.

The new bridge compiled successfully on the supplied ROCm instance. A synthetic 250K x 256, 1000-query development test measured build+first query 3.441 s, QPS 1443.59, Recall@10 .9677, Recall@100 .93738. Frozen baseline in that run: 19.997 s, QPS 273.996, Recall@10 .9480, Recall@100 .89846. These direct bridge results are NOT formal SQL or Hidden250K results and must not be treated as official speedup. The parent 30-iteration bridge previously measured approximately 6.2 s in separate runs; that comparison is not a paired official result.

The parent's complete seekdb compilation has now succeeded and its server started and accepted SQL. The new revision changes only the bridge iteration parameter; extensive new integration/4096-query validation was intentionally deferred at the user's request to meet the submission deadline. No claim that this revision meets the official 3x build target or all gates is made. The previous passing branch is preserved for rollback. REPORT.md and results/v2 describe the parent, not this revision.