#include <deque>
#include <boost/thread.hpp>
#include <boost/noncopyable.hpp>
#include <boost/atomic.hpp>

class CServiceFunctionHandler
{
public:
	virtual void OnCallFunction() {}
	virtual void OnReturnFunction() {}
};

class CServiceFunction : boost::noncopyable
{
private:
	CServiceFunctionHandler *m_pHandler;
	boost::detail::thread_data_ptr m_Context;

public:
	template<typename C>
	void Initialize(BOOST_THREAD_RV_REF(C) f) {
		m_Context = GetContext(boost::thread_detail::decay_copy(boost::forward<C>(f)));
	}

	template<typename C>
	explicit CServiceFunction(BOOST_THREAD_RV_REF(C) f) : m_pHandler(NULL) {
		Initialize(f);
	}

	template<typename C>
	explicit CServiceFunction(BOOST_THREAD_RV_REF(C) f, CServiceFunctionHandler *pHandler) : m_pHandler(pHandler) {
		Initialize(f);
	}

	template<class T, class... Args>
	explicit CServiceFunction(T f, Args&&... args) : m_pHandler(NULL) {
		Initialize(boost::bind(f, std::forward<Args>(args)...));
	}

	template<class T, class... Args>
	explicit CServiceFunction(CServiceFunctionHandler *pHandler, T f, Args&&... args) : m_pHandler(pHandler) {
		Initialize(boost::bind(f, std::forward<Args>(args)...));
	}

	template<class C>
	explicit CServiceFunction(boost::thread::attributes &attr, BOOST_THREAD_RV_REF(C) f) {
		m_Context = boost::thread_detail::decay_copy(boost::forward<C>(GetContext(f)));
	}

	template<typename C>
	static inline boost::detail::thread_data_ptr GetContext(BOOST_THREAD_RV_REF(C) f) {
		return boost::detail::thread_data_ptr(boost::detail::heap_new<boost::detail::thread_data<typename boost::remove_reference<C>::type> >(
			boost::forward<C>(f)));
	}

	inline void operator()() {
		Run();
	}

	inline void Release() {
		delete this;
	}

	inline void Run() {
		m_pHandler->OnCallFunction();
		m_Context->run();
		m_pHandler->OnReturnFunction();
	}

private:
	virtual ~CServiceFunction() {
	}
};

class CServiceThread : boost::noncopyable
{
private:
	std::deque<CServiceFunction*> m_WorkQueue;

	boost::thread				m_Thread;
	boost::mutex				m_Mutex;
	boost::condition_variable	m_Condition;

	boost::atomics::atomic_bool m_bBusy;
	boost::atomics::atomic_bool m_bWork;
	boost::atomics::atomic_bool m_bDeleteLater;

	int SafeRelease() 
	{
		delete this;
		return 0;
	}

	virtual ~CServiceThread()
	{
		Clear();
	}

	void Clear()
	{
		while (!m_WorkQueue.empty())
		{
			m_WorkQueue[0]->Release();
			m_WorkQueue.pop_back();
		}
	}

public:
	CServiceThread() :
		m_bBusy(false),
		m_bWork(true),
		m_bDeleteLater(false)
	{
		m_Thread = boost::thread(boost::bind(&CServiceThread::Wait, this));
	}

	void Release()
	{
		m_bWork = false;
		m_Condition.notify_all();
	}

	void ReleaseAfterWork()
	{
		m_bDeleteLater = true;
		m_Condition.notify_all();
	}

	int Wait()
	{
		boost::mutex::scoped_lock lock(m_Mutex);

		while (m_bWork)
		{
			if (!lock.owns_lock())
			{
				lock.lock();
			}

			m_Condition.wait(lock, [this]
			{
				return !m_WorkQueue.empty()
					|| !!m_bDeleteLater
					|| !m_bWork;
			});

			if (m_bDeleteLater && m_WorkQueue.empty())
			{
				lock.unlock();
				return SafeRelease();
			}

			if (!m_bWork)
			{
				lock.unlock();
				return SafeRelease();
			}

			auto f = m_WorkQueue[0];

			m_WorkQueue.pop_front();
			lock.unlock();

			f->Run();
			f->Release();
		}

		lock.unlock();
		return SafeRelease();
	}

	template<class C>
	bool TryPost(BOOST_THREAD_RV_REF(C) f)
	{
		boost::mutex::scoped_lock lock(m_Mutex, boost::try_to_lock);

		if (lock)
		{
			if (!m_bDeleteLater && m_bWork)
			{
				CServiceFunction *pFunction = new CServiceFunction(f);

				m_WorkQueue.push_back(pFunction);
				m_Condition.notify_all();
			}

			return true;
		}

		return false;
	}

	template<class C>
	void Post(BOOST_THREAD_RV_REF(C) f)
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		{
			if (!m_bDeleteLater && m_bWork)
			{
				CServiceFunction *pFunction = new CServiceFunction(f);

				m_WorkQueue.push_back(pFunction);
				m_Condition.notify_all();
			}
		}
	}

	template<class C>
	void Post(BOOST_THREAD_RV_REF(C) f, CServiceFunctionHandler *pHandler)
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		{
			if (!m_bDeleteLater && m_bWork)
			{
				CServiceFunction *pFunction = new CServiceFunction((BOOST_THREAD_RV_REF(C))f, pHandler);

				m_WorkQueue.push_back(pFunction);
				m_Condition.notify_all();
			}
		}
	}

	template<typename T, typename ...Args>
	void Post(CServiceFunctionHandler *pHandler, T(*func)(Args...), Args&&... params)
	{
		boost::mutex::scoped_lock lock(m_Mutex);
		{
			if (!m_bDeleteLater && m_bWork)
			{
				CServiceFunction *pFunction = new CServiceFunction(boost::bind(&func, params...), pHandler);

				m_WorkQueue.push_back(pFunction);
				m_Condition.notify_all();
			}
		}
	}
};