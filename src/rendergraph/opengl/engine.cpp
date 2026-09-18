#include "rendergraph/engine.h"

#include <QDebug>
#include <cassert>

#include "rendergraph/node.h"

using namespace rendergraph;

Engine::Engine(std::unique_ptr<BaseNode> pRootNode)
        : m_pRootNode(std::move(pRootNode)) {
    add(m_pRootNode.get());
}

Engine::~Engine() {
    // Explicitly remove the root node (and tree from the engine before deallocating its vectors)
    remove(m_pRootNode.get());
}

void Engine::add(BaseNode* pNode) {
    assert(pNode->engine() == nullptr || pNode->engine() == this);
    if (pNode->engine() == nullptr) {
        pNode->setEngine(this);
        m_pInitializeNodes.push_back(pNode);
        if (pNode->usePreprocess()) {
            m_pPreprocessNodes.push_back(pNode);
        }
        pNode = pNode->firstChild();
        while (pNode) {
            add(pNode);
            pNode = pNode->nextSibling();
        }
    }
}

void Engine::remove(BaseNode* pNode) {
    assert(pNode->engine() == this);
    pNode->setEngine(nullptr);

    std::erase(m_pInitializeNodes, pNode);
    std::erase(m_pPreprocessNodes, pNode);

    if (m_pRootNode.get() == pNode) {
        m_pRootNode.reset();
    }
}

void Engine::render() {
    if (!m_pInitializeNodes.empty()) {
        for (auto* pNode : m_pInitializeNodes) {
            pNode->initialize();
        }
        m_pInitializeNodes.clear();
    }
    if (m_pRootNode && !m_pRootNode->isSubtreeBlocked()) {
        render(m_pRootNode.get());
    }
}

void Engine::render(BaseNode* pNode) {
    pNode->render();
    pNode = pNode->firstChild();
    while (pNode) {
        if (!pNode->isSubtreeBlocked()) {
            render(pNode);
        }
        pNode = pNode->nextSibling();
    }
}

void Engine::preprocess() {
    for (auto* pNode : m_pPreprocessNodes) {
        if (!pNode->isSubtreeBlocked()) {
            pNode->preprocess();
        }
    }
}

void Engine::resize(int w, int h, bool quarterTurn) {
    m_matrix.setToIdentity();
    m_matrix.ortho(QRectF(0.0f, 0.0f, w, h));
    if (quarterTurn) {
        // Nodes build their geometry in (length, breadth) space -- along the
        // axis their content runs and across it -- so a client that wants that
        // content turned a quarter turn only has to turn the scene, and every
        // node goes on emitting vertices along +x as before.
        //
        // The pair maps a point (u, v) to (w - v, u): the length axis becomes
        // the widget's y, the breadth axis its x. Breadth comes out mirrored,
        // which is invisible on content symmetric across it and is the
        // direction the alignment flags on the rest are resolved against.
        m_matrix.rotate(90.f, 0.0f, 0.0f, 1.0f);
        m_matrix.translate(0.f, -w, 0.f);
    }

    if (m_pRootNode) {
        // A node that cares about its drawing area wants it in the space its
        // geometry is in, so hand on the turned extent rather than the widget's.
        resize(m_pRootNode.get(), quarterTurn ? h : w, quarterTurn ? w : h);
    }
}

void Engine::resize(BaseNode* pNode, int w, int h) {
    pNode->resize(w, h);
    pNode = pNode->firstChild();
    while (pNode) {
        resize(pNode, w, h);
        pNode = pNode->nextSibling();
    }
}
