# CSUDB Product Roadmap

Future clients should connect through `DatabaseService` and consume `QueryResult`; none should invoke Parser or Executor directly.

| Extension | Intended integration point |
| --- | --- |
| Web Console | HTTP/WebSocket adapter above DatabaseService |
| HTTP/REST API | Protocol adapter mapping auth/session/query DTOs |
| WebSocket query streaming | Streaming evolution of QueryResult rows |
| CSUDB JDBC Driver | Native protocol driver; prepared-statement API first |
| MySQL JDBC compatibility | Existing MySQL communicator after compatibility tests |
| Python / Go drivers | Native protocol and stable DTO schema |
| TLS | Transport layer in front of native authentication |
| PreparedStatement | DatabaseService + SQL engine plan/bind lifecycle |
| Connection pool | Client/driver layer; no executor duplication |
| Role-based access control | Extension of SystemCatalog privilege subjects |
| Audit log | Auth/service decision boundary with password redaction |
| Admin dashboard | ServerInfo and BufferPoolSnapshot DTOs |

Storage extensions continue from `ReplacementPolicy`, `PageIOBackend`, `BufferPoolSnapshot`, and `flush_dirty_pages()`: mmap/O_DIRECT backends, LRU-K/2Q/ARC, background cleaning, prefetch, and async writers are intentionally not implemented in the product-shell phase.
