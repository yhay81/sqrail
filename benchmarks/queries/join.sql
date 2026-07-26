SELECT
  dim.drug_class,
  count(*) AS observations,
  sum(round(fact.value * 1000000)::BIGINT) AS total_micro_units
FROM fact
JOIN dim USING (drug_id)
GROUP BY dim.drug_class
ORDER BY dim.drug_class
