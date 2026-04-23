# worker/engine/validator.py

import os
import struct
from pathlib import Path
from dataclasses import dataclass, field
from typing import Optional

# ──────────────────────────────────────────
# 파일 타입별 매직 바이트 시그니처 정의
# Scalpel이 지원하는 주요 포맷 기준
# ──────────────────────────────────────────
MAGIC_SIGNATURES = {
    "jpg":  [(0, b'\xFF\xD8\xFF')],
    "png":  [(0, b'\x89PNG\r\n\x1a\n')],
    "pdf":  [(0, b'%PDF')],
    "zip":  [(0, b'PK\x03\x04')],
    "gif":  [(0, b'GIF87a'), (0, b'GIF89a')],
    "bmp":  [(0, b'BM')],
    "mp4":  [(4, b'ftyp')],
    "avi":  [(0, b'RIFF')],
    "docx": [(0, b'PK\x03\x04')],   # zip 기반
    "exe":  [(0, b'MZ')],
}

# 파일 타입별 최소 유효 크기 (bytes)
MIN_VALID_SIZE = {
    "jpg": 128,
    "png": 67,
    "pdf": 64,
    "zip": 22,
    "gif": 35,
    "bmp": 54,
    "mp4": 32,
    "avi": 64,
    "exe": 64,
}


# ──────────────────────────────────────────
# 검증 결과 데이터 클래스
# ──────────────────────────────────────────
@dataclass
class ValidationResult:
    file_path:      str
    file_name:      str
    file_size:      int
    detected_type:  Optional[str]           # 매직 바이트로 감지된 타입
    claimed_type:   Optional[str]           # 파일 확장자 기반 타입
    is_valid:       bool                    # 최종 유효 여부
    checks:         dict = field(default_factory=dict)  # 세부 검사 결과
    error:          Optional[str] = None    # 오류 메시지


class Validator:
    """
    Scalpel 복구 파일에 대한 후처리 검증 모듈.
    
    검증 항목:
        1. 파일 존재 및 크기 확인
        2. 매직 바이트 검증 (파일 시그니처)
        3. 확장자 vs 실제 타입 일치 여부
        4. 최소 크기 기준 통과 여부
        5. 파일 구조 무결성 (포맷별 간이 검사)
    
    팀원 파트 확장 포인트:
        - _check_structure() 내부에 포맷별 정밀 검증 로직 추가
        - 추가 시그니처 등록 (MAGIC_SIGNATURES 확장)
    """

    def validate(self, file_path: str, claimed_type: str = None) -> ValidationResult:
        """
        단일 파일 검증 진입점.
        
        Args:
            file_path:    검증할 파일의 절대 경로
            claimed_type: Scalpel이 분류한 파일 타입 (확장자 기반)
        
        Returns:
            ValidationResult 객체
        """
        path = Path(file_path)

        # 1. 파일 존재 확인
        if not path.exists():
            return ValidationResult(
                file_path=file_path, file_name=path.name,
                file_size=0, detected_type=None,
                claimed_type=claimed_type, is_valid=False,
                error="File not found"
            )

        file_size = path.stat().st_size
        checks = {}

        # 2. 빈 파일 확인
        checks["not_empty"] = file_size > 0
        if not checks["not_empty"]:
            return ValidationResult(
                file_path=file_path, file_name=path.name,
                file_size=0, detected_type=None,
                claimed_type=claimed_type, is_valid=False,
                error="Empty file"
            )

        data = path.read_bytes()

        # 3. 매직 바이트로 실제 타입 감지
        detected_type = self._detect_type(data)
        checks["magic_detected"] = detected_type is not None

        # 4. 확장자와 실제 타입 일치 여부
        ext_type = claimed_type or path.suffix.lstrip('.').lower()
        checks["type_match"] = (
            detected_type == ext_type
            if detected_type and ext_type
            else None   # 판단 불가
        )

        # 5. 최소 크기 기준
        min_size = MIN_VALID_SIZE.get(detected_type or ext_type, 0)
        checks["min_size_pass"] = file_size >= min_size

        # 6. 포맷별 구조 무결성 검사
        checks["structure_ok"] = self._check_structure(data, detected_type)

        # 최종 유효 판정
        is_valid = (
            checks["not_empty"] and
            checks["magic_detected"] and
            checks["min_size_pass"] and
            checks["structure_ok"]
        )

        return ValidationResult(
            file_path=file_path,
            file_name=path.name,
            file_size=file_size,
            detected_type=detected_type,
            claimed_type=ext_type,
            is_valid=is_valid,
            checks=checks
        )

    def validate_batch(self, file_paths: list[str], claimed_type: str = None) -> list[ValidationResult]:
        """
        복수 파일 일괄 검증.
        Worker에서 Scalpel 결과 디렉토리 전체를 넘길 때 사용.
        """
        return [self.validate(fp, claimed_type) for fp in file_paths]

    # ──────────────────────────────────────────
    # 내부 메서드
    # ──────────────────────────────────────────

    def _detect_type(self, data: bytes) -> Optional[str]:
        """매직 바이트로 파일 타입 감지"""
        for file_type, sigs in MAGIC_SIGNATURES.items():
            for offset, magic in sigs:
                if data[offset:offset + len(magic)] == magic:
                    return file_type
        return None

    def _check_structure(self, data: bytes, file_type: Optional[str]) -> bool:
        """
        포맷별 간이 구조 무결성 검사.
        
        ※ 팀원 파트 핵심 확장 포인트 ※
        각 포맷별 정밀 파서 로직을 여기에 추가하면 됨.
        """
        if file_type is None:
            return False

        try:
            if file_type == "jpg":
                return self._check_jpg(data)
            elif file_type == "png":
                return self._check_png(data)
            elif file_type == "pdf":
                return self._check_pdf(data)
            elif file_type == "zip":
                return self._check_zip(data)
            else:
                # 구조 검사 미구현 타입 → 매직 바이트만으로 통과
                return True
        except Exception:
            return False

    def _check_jpg(self, data: bytes) -> bool:
        """JPG: SOI(FFD8) 시작 + EOI(FFD9) 종료 확인"""
        return data[:2] == b'\xFF\xD8' and data[-2:] == b'\xFF\xD9'

    def _check_png(self, data: bytes) -> bool:
        """PNG: 시그니처 + IEND 청크 존재 확인"""
        has_sig = data[:8] == b'\x89PNG\r\n\x1a\n'
        has_iend = b'IEND' in data[-12:]
        return has_sig and has_iend

    def _check_pdf(self, data: bytes) -> bool:
        """PDF: 헤더 + EOF 마커 확인"""
        has_header = data[:4] == b'%PDF'
        has_eof = b'%%EOF' in data[-16:]
        return has_header and has_eof

    def _check_zip(self, data: bytes) -> bool:
        """ZIP: Local File Header + End of Central Directory 확인"""
        has_local = data[:4] == b'PK\x03\x04'
        has_eocd = b'PK\x05\x06' in data
        return has_local and has_eocd