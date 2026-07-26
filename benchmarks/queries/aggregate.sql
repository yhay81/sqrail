SELECT
  drug_id,
  count(*) AS observations,
  sum(round(value * 1000000)::BIGINT) AS total_micro_units
FROM fact
GROUP BY drug_id
ORDER BY drug_id
