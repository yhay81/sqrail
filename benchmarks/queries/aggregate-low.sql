SELECT
  drug_id % 32 AS drug_class_id,
  count(*) AS observations,
  sum(round(value * 1000000)::BIGINT) AS total_micro_units
FROM fact
GROUP BY drug_class_id
ORDER BY drug_class_id
