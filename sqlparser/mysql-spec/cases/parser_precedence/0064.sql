select A, B, C, (A OR B) AND C, A OR (B AND C), A OR B AND C
  from t1_30237_bool order by A, B, C;
