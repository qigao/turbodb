select count(*) from t1_30237_bool
  where ((A XOR B) XOR C) != (A XOR (B XOR C));
