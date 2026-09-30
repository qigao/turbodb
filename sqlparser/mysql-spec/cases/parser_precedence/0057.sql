select A, B, C, (A AND B) AND C, A AND (B AND C), A AND B AND C
  from t1_30237_bool order by A, B, C;
