-- migrate:up

-- Step 9 (reference-data CRUD): names are unique per user, case-insensitively. Step 11 embeds
-- category and tag names as JSON-schema enums in the scanner's output schema, where a duplicate
-- name is ambiguous at best. Categories are unique per (user, is_income) because the scanner
-- only ever embeds one side's subset (expense for receipts, income for payslips).
CREATE UNIQUE INDEX uq_categories_user_name ON categories(user_id, is_income, lower(name));
CREATE UNIQUE INDEX uq_tags_user_name ON tags(user_id, lower(name));
CREATE UNIQUE INDEX uq_payment_methods_user_name ON payment_methods(user_id, lower(name));

-- migrate:down

DROP INDEX IF EXISTS uq_payment_methods_user_name;
DROP INDEX IF EXISTS uq_tags_user_name;
DROP INDEX IF EXISTS uq_categories_user_name;
