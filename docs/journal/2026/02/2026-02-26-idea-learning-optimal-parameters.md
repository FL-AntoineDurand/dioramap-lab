---
id: "2026-02-26-idea-learning-optimal-parameters"
date: 2026-02-26
title: "Idea: Learning Optimal Parameters from Training Set"
tags: [idea, optimization, bayesian, grid-search, overfitting, research]
category: idea
status: proposed
---

# Idea: Learning Optimal Parameters from Training Set

## What
Use Bayesian optimization or grid search to find the best parameter set for each roof type category, using the 240 synthetic + 40 real test cases as the training set.

## Why
Parameter sensitivity analysis showed only +0.07 max from single-parameter tuning. But multi-dimensional search might find better parameter combinations.

## Caveat
With only 240 training cases, overfitting is a real risk. Would need cross-validation (5-fold over roof types) or a held-out test set (200 train + 40 test).

## How
Script `scripts/optimize-params.js` that runs the pipeline with different parameter grids, scores against ground truth, and reports Pareto-optimal parameter sets per roof type.

## Difficulty
Medium (framework), high (avoiding overfitting).

## Timeline
3-5 days.
