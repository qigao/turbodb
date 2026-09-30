select A, B, C, (A XOR B) XOR C, A XOR (B XOR C), A XOR B XOR C
  from t1_30237_bool order by A, B, C;
