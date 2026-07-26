SELECT event_id, drug_id, event_date, value
FROM fact
WHERE value >= 0.99
ORDER BY event_id
