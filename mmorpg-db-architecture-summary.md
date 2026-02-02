# High-Performance MMORPG를 위한 DB 아키텍처
## Async Write-Behind + Local WAL(Write Ahead Log)

---

## 1. 목적 및 핵심 철학

클라우드 환경에서 발생할 수 있는 예측 불가능한 DB 장애(30분 이상의 연결 단절 등)와 네트워크 레이턴시로부터 게임 서버의 성능을 보호하기 위한 데이터베이스 아키텍처.

**핵심 원칙**:
- **"메모리가 Master, DB는 Slave"** - 메모리 → DB 저장 시 100% 보장
- **Fault Tolerance** - 외부 DB가 죽어도 게임 서비스는 멈추지 않음
- **Local First** - 로컬 저장소 우선 조회로 Consistency Gap 해결

---
## 2. 아키텍처 장점

- **30분 장애**: Local SQLite 파일 용량만 늘어나고 서버는 정상 동작
- **프로시저**: C++ 로직이 메인이므로 단순 SQL만 사용 가능
- **복잡도**: "DB 실패 시 롤백" 코드가 사라져 로직이 매우 단순해짐
- **로그인**: DB 장애 시에도 로컬 캐시로 즉시 로그인 성공

---

## 3. 아키텍처 흐름

```
Game Logic (Memory) 
    ↓ (즉시 성공)
Local SQLite Queue (파일 저장)
    ↓ (백그라운드 동기화)
Cloud RDBMS
```

**저장 프로세스**:
1. 유저 액션 → 메모리 즉시 반영 (Non-blocking)
2. 변경사항을 Local SQLite에 저장 (파일 큐)
3. 백그라운드 Worker가 Cloud DB로 비동기 전송
4. 성공 시 로컬 큐에서 삭제, 실패 시 재시도

**용량**: 1 Command 당 200 Byte × 초당 1000건 × 30분 = **약 360MB**

---

## 4. Consistency Gap 문제 해법(3단계 조회)

**문제**: "메모리에는 없고, 로컬 큐에는 있고, Cloud DB에는 아직 없는" 데이터

**해결**: Tiered Lookup

1. **Tier 1 (Memory)**: SessionManager 확인 → 있으면 즉시 반환
2. **Tier 2 (Local SQLite)**: PendingUsers 조회 → 있으면 로드 (DB 장애 시 핵심)
3. **Tier 3 (Cloud DB)**: 원격 DB 조회 → 일반적인 경로

→ **DB 장애 시에도 Tier 2에서 최신 데이터로 로그인 성공**

---

## 5. 핵심 장점 및 해결되는 문제들
### 5.1. 서버 크래시
서버 크래시SQLite 파일은 디스크에 보존 → 재시작 시 자동 복구 → 데이터 유실 0%
### 5.2 Cloud DB 장애 (30분 이상 단절)
로컬 디스크에 파일만 계속 쌓임 (초당 1000건시 360MB/30분),장애 회복 후 Worker의 빠른 복구, 유저는 중단 없이 게임 플레이
### 5.3 프로시저(SP) 의존성 감소
복잡한 연산은 C++에서, DB에는 단순 INSERT/UPDATE만 사용
### 5.4 메모리-DB 동기화 복잡성 제거
롤백 로직, 2-Phase Commit 등의 복잡성  해결
### 5.5 읽기(Login)와 쓰기(Save)의 분리
로그인 (SELECT), 저장 (INSERT/UPDATE)
### 5.6. 확장성
단일 서버 → 분산 서버 PendingUsers 역할을 Redis사용
