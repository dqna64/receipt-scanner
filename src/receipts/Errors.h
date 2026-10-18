#pragma once

#include <stdexcept>
#include <string>

namespace receipt_scanner::receipts {

// Controllers map these to HTTP statuses (controllers/Responses.h): NotFound -> 404 (spec API
// conventions: missing and not-owned are indistinguishable), Validation -> 400, Conflict -> 409.

class NotFoundError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ReceiptNotFoundError : public NotFoundError {
public:
  ReceiptNotFoundError() : NotFoundError("receipt not found") {}
};

// what() is shown to the client verbatim -- keep it a user-facing message naming the field.
class ValidationError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class ConflictError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class TooManyImagesError : public std::runtime_error {
public:
  TooManyImagesError() : std::runtime_error("image cap exceeded") {}
};

} // namespace receipt_scanner::receipts
