Authors: Andrew Pan, Helen Liu, Frances Adiwijaya
Date: May 1 2026

This folder contains all modified and new files from the scarab source code.
Git repository link: https://github.com/andrewpannn/scarab

Modified Files:

src/cmp_model.c - issue RFP requests whenever port is available

src/dcache_stage.c - completes memory access with 0 latency if RFP completes before read

src/decoupled_frontend.cc - model oracle and simple predictor by sending flag to back end

src/map_rename.c - launches RFP after map_rename

src/memory/mem_req.h - add additional parameters to memory request struct

src/memory/memory.c - handles RFP requests once they become memory requests

src/memory/memory.h - add extra parameters to memory system

src/memory/memory.param.def - define parameters

src/memory/memory.stat.def - define statistics to collect

src/op.h - add flag for rfp complete and eligible

src/op_pool.c - add flag for rfp complete and eligible

src/ramulator.cc - add support for MRT_RFP type

New Files:

src/memory/rfp.c - implementation of RFP launch, stride predictor, and priority queue

src/memory/rfp.h - rfp header file

src/memory/hit_pred.c - implementation of RFP hit predictor

src/memory/hit_pred.h - hit predictor header file

turnin/diff.txt - diff between original and final commit