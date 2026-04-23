# worker/engine/ai_scorer.py
'''
카빙된 결과 중에서 덜 쓸모있는 것을 빠르게 거르기 
'''
import numpy as np
from pathlib import Path

class AIScorer:
    def score(self, file_path: str) -> dict:
        data = Path(file_path).read_bytes()
        
        # 1. 바이트 빈도 분포 (BFD)
        freq = np.bincount(np.frombuffer(data, dtype=np.uint8), minlength=256)
        prob = freq / len(data)
        
        # 2. 샤논 엔트로피
        entropy = -np.sum(prob * np.log2(prob + 1e-10))
        
        # 3. 특징 벡터 → 신뢰도 점수 (현재는 엔트로피 기반 휴리스틱)
        #    추후 ML 모델로 교체 가능한 구조
        score = self._entropy_to_score(entropy)
        label = "valid" if score >= 0.5 else "suspicious"
        
        return {"score": score, "label": label, "entropy": entropy}
    
    def _entropy_to_score(self, entropy: float) -> float:
        # 엔트로피 0~8 범위를 0~1로 정규화 (단순 휴리스틱)
        return min(entropy / 8.0, 1.0)
