select A, B, C, (A AND B) OR C, A AND (B OR C), A AND B OR C
  from t1_30237_bool order by A, B, C;
