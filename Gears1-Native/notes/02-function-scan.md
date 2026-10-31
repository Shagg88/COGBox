# 02 - Finding functions (2026-10-30)

[V] tools/funcscan.py on the TU5 image: 37,942 entry candidates (pdata 27,583, bl 16,738, data pointers 18,661,
lis+addi 3,084); 8,041 reached only indirectly, 4,433 of those outside every .pdata function.

- Codegen (job 670) needed three tail-call targets; job 671 compiled (5.4 min, 62 MB exe). It warned that
  function 0x825EDB10 is 5,231,675 bytes: discovery ran it over 5,110 .pdata functions (its own .pdata entry is
  0x55F4C long). Watch for crashes inside that range (0x825EDB10-0x82AEAF4B); give it an end if needed.
- First boot (job 673): FATAL unregistered function 0x82512E08 on both renderers, 4 s in, right after the update's
  files were listed. 0x82512E08 follows a bctr, has no .pdata entry, and is built with lis+addi at 0x8228B320.
- Batch 1 (job 675): all 4,429 indirect-only, outside-.pdata candidates in code that directly follow a terminator
  (blr, bctr, b, or zero padding). Every candidate passed that test, which says the scan's evidence is good.
